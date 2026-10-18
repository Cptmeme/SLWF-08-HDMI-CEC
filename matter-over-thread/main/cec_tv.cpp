#include "cec_tv.h"

#include <atomic>
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <esp_matter.h>
#include <nvs.h>
#if CONFIG_ENABLE_CHIP_SHELL
#include <esp_matter_console.h>
#endif

#include <app/CommandResponseHelper.h>
#include <app/reporting/reporting.h>
#include <clusters/MediaPlayback/Metadata.h>
#include <lib/support/TypeTraits.h>
#include <platform/CHIPDeviceLayer.h>

#include "cec_bus.h"

using namespace chip::app::Clusters;
using chip::to_underlying;
using chip::app::AttributeValueEncoder;
using chip::app::CommandResponseHelper;
using chip::DeviceLayer::PlatformMgr;
using KeyCode = KeypadInput::CECKeyCodeEnum;
using PlaybackState = MediaPlayback::PlaybackStateEnum;

static const char *TAG = "cec_tv";

// Logical addresses and opcodes (HDMI 1.4b, CEC 10 and CEC 15)
#define CEC_TV 0x0
#define CEC_UNREGISTERED 0xF
#define CEC_DEVICE_TYPE_PLAYBACK 4

enum : uint8_t {
    CEC_FEATURE_ABORT = 0x00,
    CEC_IMAGE_VIEW_ON = 0x04,
    CEC_TEXT_VIEW_ON = 0x0D,
    CEC_DECK_STATUS = 0x1B,
    CEC_STANDBY = 0x36,
    CEC_USER_CONTROL_PRESSED = 0x44,
    CEC_USER_CONTROL_RELEASED = 0x45,
    CEC_GIVE_OSD_NAME = 0x46,
    CEC_SET_OSD_NAME = 0x47,
    CEC_SET_SYSTEM_AUDIO_MODE = 0x72,
    CEC_REPORT_AUDIO_STATUS = 0x7A,
    CEC_SYSTEM_AUDIO_MODE_STATUS = 0x7E,
    CEC_ROUTING_CHANGE = 0x80,
    CEC_ROUTING_INFORMATION = 0x81,
    CEC_ACTIVE_SOURCE = 0x82,
    CEC_GIVE_PHYSICAL_ADDRESS = 0x83,
    CEC_REPORT_PHYSICAL_ADDRESS = 0x84,
    CEC_SET_STREAM_PATH = 0x86,
    CEC_DEVICE_VENDOR_ID = 0x87,
    CEC_GIVE_DEVICE_VENDOR_ID = 0x8C,
    CEC_MENU_STATUS = 0x8E,
    CEC_GIVE_DEVICE_POWER_STATUS = 0x8F,
    CEC_REPORT_POWER_STATUS = 0x90,
    CEC_CEC_VERSION = 0x9E,
    CEC_GET_CEC_VERSION = 0x9F,
    CEC_ABORT = 0xFF,
};

#define CEC_ABORT_UNRECOGNIZED_OPCODE 0x00
#define CEC_ABORT_REFUSED 0x04
#define CEC_VERSION_1_4 0x05
#define CEC_POWER_ON 0x00
#define CEC_POWER_TURNING_ON 0x02

// The input index is the TV port: 0 is the TV's own tuner, 1 is HDMI 1, and so on
#define INPUT_COUNT (CONFIG_CEC_HDMI_INPUTS + 1)
#define INPUT_NAME_SIZE 33

#define POWER_POLL_INTERVAL pdMS_TO_TICKS(30000)

static const uint8_t k_playback_addresses[] = {4, 8, 11};

static uint16_t s_endpoint;
static QueueHandle_t s_events;
static std::atomic<uint8_t> s_address{CEC_UNREGISTERED};
static std::atomic<bool> s_tv_on{false};
static std::atomic<uint8_t> s_current_input{0};
static std::atomic<uint8_t> s_playback_state{to_underlying(PlaybackState::kNotPlaying)};
static std::atomic<uint8_t> s_port_source[16]; // logical address of the source on each TV port
static char s_input_names[INPUT_COUNT][INPUT_NAME_SIZE];

static void send(uint8_t destination, const uint8_t *data, size_t length)
{
    cec_frame_t frame = {};
    frame.blocks[0] = s_address << 4 | destination;
    memcpy(&frame.blocks[1], data, length);
    frame.length = length + 1;
    if (!cec_bus_send(&frame)) {
        ESP_LOGW(TAG, "transmit queue full, dropped %02X:%02X", frame.blocks[0], frame.blocks[1]);
    }
}

static void send(uint8_t destination, std::initializer_list<uint8_t> data)
{
    send(destination, data.begin(), data.size());
}

static void press_key(uint8_t destination, uint8_t key)
{
    send(destination, {CEC_USER_CONTROL_PRESSED, key});
    send(destination, {CEC_USER_CONTROL_RELEASED});
}

static void report_physical_address()
{
    send(CEC_BROADCAST, {CEC_REPORT_PHYSICAL_ADDRESS, CONFIG_CEC_PHYSICAL_ADDRESS >> 8,
                         CONFIG_CEC_PHYSICAL_ADDRESS & 0xFF, CEC_DEVICE_TYPE_PLAYBACK});
}

static void send_osd_name(uint8_t destination)
{
    cec_frame_t frame = {};
    frame.blocks[0] = s_address << 4 | destination;
    frame.blocks[1] = CEC_SET_OSD_NAME;
    constexpr size_t length = sizeof(CONFIG_CEC_OSD_NAME) - 1 < 14 ? sizeof(CONFIG_CEC_OSD_NAME) - 1 : 14;
    memcpy(&frame.blocks[2], CONFIG_CEC_OSD_NAME, length);
    frame.length = 2 + length;
    cec_bus_send(&frame);
}

// Attribute reports run on the Matter thread

static void report_on_off(intptr_t)
{
    esp_matter_attr_val_t val = esp_matter_bool(s_tv_on);
    esp_matter::attribute::report(s_endpoint, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
}

static void report_current_input(intptr_t)
{
    MatterReportingAttributeChangeCallback(s_endpoint, MediaInput::Id, MediaInput::Attributes::CurrentInput::Id);
}

static void report_input_list(intptr_t)
{
    MatterReportingAttributeChangeCallback(s_endpoint, MediaInput::Id, MediaInput::Attributes::InputList::Id);
}

static void report_playback_state(intptr_t)
{
    MatterReportingAttributeChangeCallback(s_endpoint, MediaPlayback::Id, MediaPlayback::Attributes::CurrentState::Id);
}

static void set_playback_state(PlaybackState state)
{
    if (s_playback_state.exchange(to_underlying(state)) != to_underlying(state)) {
        PlatformMgr().ScheduleWork(report_playback_state);
    }
}

static void set_tv_on(bool on)
{
    if (s_tv_on.exchange(on) != on) {
        ESP_LOGI(TAG, "TV turned %s", on ? "on" : "off");
        PlatformMgr().ScheduleWork(report_on_off);
    }
    if (!on) {
        set_playback_state(PlaybackState::kNotPlaying);
    }
}

static void set_current_input(uint8_t index)
{
    if (s_current_input.exchange(index) != index) {
        ESP_LOGI(TAG, "TV switched to %s", s_input_names[index]);
        PlatformMgr().ScheduleWork(report_current_input);
    }
}

static void follow_physical_address(uint16_t physical_address)
{
    uint8_t port = physical_address >> 12;
    if (port < INPUT_COUNT) {
        set_current_input(port);
    }
}

// Playback keys go to the source on the current input, or to the TV when that source is unknown
static uint8_t playback_target()
{
    uint8_t input = s_current_input;
    uint8_t address = input ? s_port_source[input].load() : CEC_UNREGISTERED;
    return address == CEC_UNREGISTERED ? CEC_TV : address;
}

// Replies to our own requests, and keys forwarded by the TV, never get a <Feature Abort>
static bool is_reply_or_key(uint8_t opcode)
{
    switch (opcode) {
    case CEC_FEATURE_ABORT:
    case CEC_DECK_STATUS:
    case CEC_STANDBY:
    case CEC_USER_CONTROL_PRESSED:
    case CEC_USER_CONTROL_RELEASED:
    case CEC_SET_OSD_NAME:
    case CEC_SET_SYSTEM_AUDIO_MODE:
    case CEC_REPORT_AUDIO_STATUS:
    case CEC_SYSTEM_AUDIO_MODE_STATUS:
    case CEC_REPORT_PHYSICAL_ADDRESS:
    case CEC_DEVICE_VENDOR_ID:
    case CEC_MENU_STATUS:
    case CEC_REPORT_POWER_STATUS:
    case CEC_CEC_VERSION:
        return true;
    default:
        return false;
    }
}

// Answer a message sent directly to us
static void respond(uint8_t initiator, uint8_t opcode)
{
    switch (opcode) {
    case CEC_GIVE_PHYSICAL_ADDRESS:
        report_physical_address();
        break;
    case CEC_GIVE_DEVICE_VENDOR_ID:
        send(CEC_BROADCAST, {CEC_DEVICE_VENDOR_ID, 0x00, 0x00, 0x00});
        break;
    case CEC_GIVE_OSD_NAME:
        send_osd_name(initiator);
        break;
    case CEC_GET_CEC_VERSION:
        send(initiator, {CEC_CEC_VERSION, CEC_VERSION_1_4});
        break;
    case CEC_GIVE_DEVICE_POWER_STATUS:
        send(initiator, {CEC_REPORT_POWER_STATUS, CEC_POWER_ON});
        break;
    case CEC_ABORT:
        send(initiator, {CEC_FEATURE_ABORT, opcode, CEC_ABORT_REFUSED});
        break;
    default:
        if (!is_reply_or_key(opcode) && initiator != CEC_UNREGISTERED) {
            send(initiator, {CEC_FEATURE_ABORT, opcode, CEC_ABORT_UNRECOGNIZED_OPCODE});
        }
        break;
    }
}

// Follow the TV's power and input from everything on the bus
static void follow(uint8_t initiator, uint8_t destination, uint8_t opcode, const uint8_t *operands, size_t count)
{
    switch (opcode) {
    case CEC_REPORT_POWER_STATUS:
        if (initiator == CEC_TV && count >= 1) {
            set_tv_on(operands[0] == CEC_POWER_ON || operands[0] == CEC_POWER_TURNING_ON);
        }
        break;
    case CEC_STANDBY:
        if (initiator == CEC_TV || destination == CEC_TV) {
            set_tv_on(false);
        }
        break;
    case CEC_IMAGE_VIEW_ON:
    case CEC_TEXT_VIEW_ON:
        if (destination == CEC_TV) {
            set_tv_on(true);
        }
        break;
    case CEC_ACTIVE_SOURCE:
        if (count >= 2) {
            uint16_t physical_address = operands[0] << 8 | operands[1];
            if (initiator != s_address) {
                s_port_source[physical_address >> 12] = initiator;
            }
            follow_physical_address(physical_address);
        }
        break;
    case CEC_ROUTING_CHANGE:
        if (count >= 4) {
            follow_physical_address(operands[2] << 8 | operands[3]);
        }
        break;
    case CEC_ROUTING_INFORMATION:
    case CEC_SET_STREAM_PATH:
        if (count >= 2) {
            follow_physical_address(operands[0] << 8 | operands[1]);
        }
        break;
    case CEC_REPORT_PHYSICAL_ADDRESS:
        if (count >= 2 && initiator != CEC_TV) {
            s_port_source[operands[0] >> 4] = initiator;
        }
        break;
    }
}

static void handle(const cec_frame_t *frame)
{
    char hex[3 * CEC_MAX_BLOCKS] = "";
    for (int i = 0, n = 0; i < frame->length; i++) {
        n += snprintf(hex + n, sizeof(hex) - n, i ? ":%02X" : "%02X", frame->blocks[i]);
    }
    ESP_LOGD(TAG, "<< %s", hex);

    if (frame->length < 2) {
        return; // polling message
    }
    uint8_t initiator = frame->blocks[0] >> 4;
    uint8_t destination = frame->blocks[0] & 0xF;
    uint8_t opcode = frame->blocks[1];
    follow(initiator, destination, opcode, &frame->blocks[2], frame->length - 2);
    if (destination == s_address && destination != CEC_UNREGISTERED) {
        respond(initiator, opcode);
    }
}

// Wait for the result of the frame we just queued, following the bus meanwhile
static bool wait_until_sent()
{
    cec_event_t event;
    bool warned = false;
    while (true) {
        if (!xQueueReceive(s_events, &event, pdMS_TO_TICKS(2000))) {
            if (!warned) {
                cec_bus_status_t status;
                cec_bus_get_status(&status);
                ESP_LOGW(TAG, "still waiting for a free CEC bus, the line is %s", status.line_high ? "high" : "low");
                warned = true;
            }
            continue;
        }
        if (event.type == cec_event_t::SENT) {
            return event.ack;
        }
        handle(&event.frame);
    }
}

// Poll each playback address; the first one nobody acknowledges is ours (CEC 10.2.1)
static void allocate_logical_address()
{
    for (uint8_t candidate : k_playback_addresses) {
        cec_frame_t poll = {};
        poll.blocks[0] = candidate << 4 | candidate;
        poll.length = 1;
        cec_bus_send(&poll);
        if (!wait_until_sent()) {
            s_address = candidate;
            cec_bus_set_logical_address(candidate);
            ESP_LOGI(TAG, "logical address %u, physical address %04X", candidate, CONFIG_CEC_PHYSICAL_ADDRESS);
            return;
        }
    }
    ESP_LOGW(TAG, "all playback addresses are taken, staying unregistered");
}

static void cec_task(void *arg)
{
    allocate_logical_address();
    report_physical_address();

    TickType_t last_poll = xTaskGetTickCount() - POWER_POLL_INTERVAL;
    cec_event_t event;
    while (true) {
        if (xTaskGetTickCount() - last_poll >= POWER_POLL_INTERVAL) {
            last_poll = xTaskGetTickCount();
            send(CEC_TV, {CEC_GIVE_DEVICE_POWER_STATUS});
        }
        if (!xQueueReceive(s_events, &event, pdMS_TO_TICKS(1000))) {
            continue;
        }
        if (event.type == cec_event_t::RECEIVED) {
            handle(&event.frame);
        } else if (!event.ack) {
            ESP_LOGW(TAG, "%02X:%02X was not acknowledged", event.frame.blocks[0], event.frame.blocks[1]);
        }
    }
}

static void save_input_name(uint8_t index)
{
    nvs_handle_t nvs;
    if (nvs_open("cec_tv", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    char key[8];
    snprintf(key, sizeof(key), "input%u", index);
    nvs_set_str(nvs, key, s_input_names[index]);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void load_input_names()
{
    for (int i = 0; i < INPUT_COUNT; i++) {
        if (i == 0) {
            strcpy(s_input_names[i], "Live TV");
        } else {
            snprintf(s_input_names[i], INPUT_NAME_SIZE, "HDMI %d", i);
        }
    }
    nvs_handle_t nvs;
    if (nvs_open("cec_tv", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    for (int i = 0; i < INPUT_COUNT; i++) {
        char key[8];
        snprintf(key, sizeof(key), "input%d", i);
        size_t size = INPUT_NAME_SIZE;
        nvs_get_str(nvs, key, s_input_names[i], &size);
    }
    nvs_close(nvs);
}

static void power(bool on)
{
    s_tv_on = on;
    send(CEC_TV, {on ? CEC_IMAGE_VIEW_ON : CEC_STANDBY});
    if (!on) {
        set_playback_state(PlaybackState::kNotPlaying);
    }
}

class PlaybackDelegate : public MediaPlayback::Delegate {
    using Helper = CommandResponseHelper<MediaPlayback::Commands::PlaybackResponse::Type>;
    using Status = MediaPlayback::StatusEnum;

public:
    PlaybackState HandleGetCurrentState() override { return static_cast<PlaybackState>(s_playback_state.load()); }
    uint64_t HandleGetStartTime() override { return 0; }
    uint64_t HandleGetDuration() override { return 0; }
    CHIP_ERROR HandleGetSampledPosition(AttributeValueEncoder &encoder) override { return encoder.EncodeNull(); }
    float HandleGetPlaybackSpeed() override { return 0; }
    uint64_t HandleGetSeekRangeStart() override { return 0; }
    uint64_t HandleGetSeekRangeEnd() override { return 0; }
    CHIP_ERROR HandleGetActiveAudioTrack(AttributeValueEncoder &encoder) override { return encoder.EncodeNull(); }
    CHIP_ERROR HandleGetAvailableAudioTracks(AttributeValueEncoder &encoder) override { return encoder.EncodeNull(); }
    CHIP_ERROR HandleGetActiveTextTrack(AttributeValueEncoder &encoder) override { return encoder.EncodeNull(); }
    CHIP_ERROR HandleGetAvailableTextTracks(AttributeValueEncoder &encoder) override { return encoder.EncodeNull(); }

    void HandlePlay(Helper &helper) override { key(helper, KeyCode::kPlay, PlaybackState::kPlaying); }
    void HandlePause(Helper &helper) override { key(helper, KeyCode::kPause, PlaybackState::kPaused); }
    void HandleStop(Helper &helper) override { key(helper, KeyCode::kStop, PlaybackState::kNotPlaying); }
    void HandleFastForward(Helper &helper, const chip::Optional<bool> &) override
    {
        key(helper, KeyCode::kFastForward, PlaybackState::kPlaying);
    }
    void HandleRewind(Helper &helper, const chip::Optional<bool> &) override
    {
        key(helper, KeyCode::kRewind, PlaybackState::kPlaying);
    }
    void HandlePrevious(Helper &helper) override { key(helper, KeyCode::kBackward); }
    void HandleNext(Helper &helper) override { key(helper, KeyCode::kForward); }

    // CEC has no commands for these, and they are not in the AcceptedCommandList
    void HandleSkipBackward(Helper &helper, const uint64_t &) override { respond(helper, Status::kNotAllowed); }
    void HandleSkipForward(Helper &helper, const uint64_t &) override { respond(helper, Status::kNotAllowed); }
    void HandleSeek(Helper &helper, const uint64_t &) override { respond(helper, Status::kNotAllowed); }
    void HandleStartOver(Helper &helper) override { respond(helper, Status::kNotAllowed); }
    bool HandleActivateAudioTrack(const chip::CharSpan &, const uint8_t &) override { return false; }
    bool HandleActivateTextTrack(const chip::CharSpan &) override { return false; }
    bool HandleDeactivateTextTrack() override { return false; }

    uint32_t GetFeatureMap(chip::EndpointId) override { return to_underlying(MediaPlayback::Feature::kVariableSpeed); }
    uint16_t GetClusterRevision(chip::EndpointId) override { return MediaPlayback::kRevision; }

private:
    static void respond(Helper &helper, Status status)
    {
        MediaPlayback::Commands::PlaybackResponse::Type response;
        response.status = status;
        helper.Success(response);
    }

    // The state follows the last command, since most sources do not report it over CEC
    static void key(Helper &helper, KeyCode key_code, PlaybackState state)
    {
        press_key(playback_target(), to_underlying(key_code));
        set_playback_state(state);
        respond(helper, Status::kSuccess);
    }

    static void key(Helper &helper, KeyCode key_code)
    {
        press_key(playback_target(), to_underlying(key_code));
        respond(helper, Status::kSuccess);
    }
};

class InputDelegate : public MediaInput::Delegate {
public:
    CHIP_ERROR HandleGetInputList(AttributeValueEncoder &encoder) override
    {
        return encoder.EncodeList([](const auto &list) -> CHIP_ERROR {
            for (uint8_t i = 0; i < INPUT_COUNT; i++) {
                MediaInput::Structs::InputInfoStruct::Type input;
                input.index = i;
                input.inputType = i ? MediaInput::InputTypeEnum::kHdmi : MediaInput::InputTypeEnum::kInternal;
                input.name = chip::CharSpan::fromCharString(s_input_names[i]);
                input.description = chip::CharSpan::fromCharString(i ? "" : "TV tuner");
                ReturnErrorOnFailure(list.Encode(input));
            }
            return CHIP_NO_ERROR;
        });
    }

    uint8_t HandleGetCurrentInput() override { return s_current_input; }

    bool HandleSelectInput(const uint8_t index) override
    {
        if (index >= INPUT_COUNT) {
            return false;
        }
        uint8_t high = index << 4;
        if (index) {
            // Claim that port as the active source, which makes the TV switch to it
            send(CEC_BROADCAST, {CEC_ACTIVE_SOURCE, high, 0x00});
        }
        send(CEC_BROADCAST, {CEC_SET_STREAM_PATH, high, 0x00});
        set_current_input(index);
        return true;
    }

    bool HandleShowInputStatus() override
    {
        press_key(CEC_TV, to_underlying(KeyCode::kDisplayInformation));
        return true;
    }

    // The TV hides its information banner by itself
    bool HandleHideInputStatus() override { return true; }

    bool HandleRenameInput(const uint8_t index, const chip::CharSpan &name) override
    {
        if (index >= INPUT_COUNT || name.size() >= INPUT_NAME_SIZE) {
            return false;
        }
        memcpy(s_input_names[index], name.data(), name.size());
        s_input_names[index][name.size()] = '\0';
        save_input_name(index);
        PlatformMgr().ScheduleWork(report_input_list);
        return true;
    }
};

class KeypadDelegate : public KeypadInput::Delegate {
public:
    void HandleSendKey(CommandResponseHelper<KeypadInput::Commands::SendKeyResponse::Type> &helper,
                       const KeyCode &key_code) override
    {
        press_key(CEC_TV, to_underlying(key_code));
        KeypadInput::Commands::SendKeyResponse::Type response;
        response.status = KeypadInput::StatusEnum::kSuccess;
        helper.Success(response);
    }

    uint32_t GetFeatureMap(chip::EndpointId) override
    {
        return to_underlying(KeypadInput::Feature::kNavigationKeyCodes) |
            to_underlying(KeypadInput::Feature::kLocationKeys) | to_underlying(KeypadInput::Feature::kNumberKeys);
    }
};

class SleepDelegate : public LowPower::Delegate {
public:
    bool HandleSleep() override
    {
        power(false);
        PlatformMgr().ScheduleWork(report_on_off);
        return true;
    }
};

#if CONFIG_ENABLE_CHIP_SHELL
// matter esp cec                               bus status
// matter esp cec send <destination> <byte> ...  send a frame, e.g. "cec send 0 04" for <Image View On>
static esp_err_t console_cec(int argc, char **argv)
{
    if (argc == 0) {
        cec_bus_status_t status;
        cec_bus_get_status(&status);
        printf("line %s, %s%s\n", status.line_high ? "high" : "low", status.receiving ? "receiving " : "",
               status.transmitting ? "transmitting" : "");
        printf("logical address %X, physical address %X.%X.%X.%X\n", s_address.load(),
               CONFIG_CEC_PHYSICAL_ADDRESS >> 12, (CONFIG_CEC_PHYSICAL_ADDRESS >> 8) & 0xF,
               (CONFIG_CEC_PHYSICAL_ADDRESS >> 4) & 0xF, CONFIG_CEC_PHYSICAL_ADDRESS & 0xF);
        printf("frames received %lu, sent %lu, not acknowledged %lu\n", (unsigned long)status.received,
               (unsigned long)status.sent, (unsigned long)status.failed);
        printf("TV %s, input %s\n", s_tv_on ? "on" : "off", s_input_names[s_current_input]);
        return ESP_OK;
    }
    if (strcmp(argv[0], "send") == 0 && argc >= 3 && argc - 2 < CEC_MAX_BLOCKS) {
        uint8_t data[CEC_MAX_BLOCKS - 1];
        for (int i = 2; i < argc; i++) {
            data[i - 2] = strtoul(argv[i], nullptr, 16);
        }
        send(strtoul(argv[1], nullptr, 16) & 0xF, data, argc - 2);
        return ESP_OK;
    }
    printf("usage: matter esp cec [send <destination> <byte> ...]\n");
    return ESP_ERR_INVALID_ARG;
}
#endif

static PlaybackDelegate s_playback_delegate;
static InputDelegate s_input_delegate;
static KeypadDelegate s_keypad_delegate;
static SleepDelegate s_sleep_delegate;

namespace cec_tv {

void init(uint16_t endpoint_id)
{
    s_endpoint = endpoint_id;
    for (auto &source : s_port_source) {
        source = CEC_UNREGISTERED;
    }
    load_input_names();
}

void start()
{
    s_events = xQueueCreate(16, sizeof(cec_event_t));
    ESP_ERROR_CHECK(cec_bus_init((gpio_num_t)CONFIG_CEC_GPIO, s_events));
    xTaskCreate(cec_task, "cec", 4096, nullptr, 5, nullptr);
}

void set_power(bool on)
{
    power(on);
}

void register_console_commands()
{
#if CONFIG_ENABLE_CHIP_SHELL
    static const esp_matter::console::command_t command = {
        .name = "cec",
        .description = "CEC bus status, or send a frame. Usage: matter esp cec [send <destination> <byte> ...]",
        .handler = console_cec,
    };
    esp_matter::console::add_commands(&command, 1);
#endif
}

MediaPlayback::Delegate *media_playback()
{
    return &s_playback_delegate;
}

MediaInput::Delegate *media_input()
{
    return &s_input_delegate;
}

KeypadInput::Delegate *keypad_input()
{
    return &s_keypad_delegate;
}

LowPower::Delegate *low_power()
{
    return &s_sleep_delegate;
}

} // namespace cec_tv
