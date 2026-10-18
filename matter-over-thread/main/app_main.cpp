#include <esp_err.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <common_macros.h>

#include <app-common/zap-generated/callback.h>
#include <app/PluginApplicationCallbacks.h>
#include <app/data-model/Decode.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <clusters/LowPower/Metadata.h>
#include <clusters/MediaInput/Metadata.h>
#include <clusters/MediaPlayback/Metadata.h>
#include <lib/support/TypeTraits.h>
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

#include "cec_tv.h"

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace chip::app::Clusters;
using chip::to_underlying;

constexpr auto k_timeout_seconds = 300;

// Basic Video Player (Matter 1.5 Device Library, 10.2)
constexpr uint32_t k_basic_video_player_device_type_id = 0x0028;
constexpr uint8_t k_basic_video_player_device_type_version = 2;

static uint16_t s_video_player_endpoint_id;

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
        ESP_LOGI(TAG, "Fabric removed successfully");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            chip::CommissioningWindowManager &commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
            constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
            if (!commissionMgr.IsCommissioningWindowOpen()) {
                CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(kTimeoutSeconds,
                                                chip::CommissioningWindowAdvertisement::kDnssdOnly);
                if (err != CHIP_NO_ERROR) {
                    ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    return ESP_OK;
}

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    if (type == attribute::PRE_UPDATE && endpoint_id == s_video_player_endpoint_id && cluster_id == OnOff::Id &&
        attribute_id == OnOff::Attributes::OnOff::Id) {
        cec_tv::set_power(val->val.b);
    }
    return ESP_OK;
}

// Decode a command and hand it to the Matter SDK's cluster server, which calls our delegate
template <typename Decodable, bool (*Handler)(chip::app::CommandHandler *, const chip::app::ConcreteCommandPath &,
                                              const Decodable &)>
static esp_err_t dispatch(const chip::app::ConcreteCommandPath &path, chip::TLV::TLVReader &tlv, void *opaque)
{
    auto *handler = static_cast<chip::app::CommandHandler *>(opaque);
    Decodable data;
    if (chip::app::DataModel::Decode(tlv, data) != CHIP_NO_ERROR) {
        handler->AddStatus(path, chip::Protocols::InteractionModel::Status::InvalidCommand);
        return ESP_OK;
    }
    Handler(handler, path, data);
    return ESP_OK;
}

static void create_media_playback(endpoint_t *endpoint)
{
    namespace Commands = MediaPlayback::Commands;
    cluster_t *cluster = cluster::create(endpoint, MediaPlayback::Id, CLUSTER_FLAG_SERVER);
    cluster::set_plugin_server_init_callback(cluster, MatterMediaPlaybackPluginServerInitCallback);
    cluster::set_delegate_and_init_callback(
        cluster,
        [](void *delegate, uint16_t endpoint_id) {
            MediaPlayback::SetDefaultDelegate(endpoint_id, static_cast<MediaPlayback::Delegate *>(delegate));
        },
        cec_tv::media_playback());
    cluster::global::attribute::create_feature_map(cluster, to_underlying(MediaPlayback::Feature::kVariableSpeed));
    cluster::global::attribute::create_cluster_revision(cluster, MediaPlayback::kRevision);
    attribute::create(cluster, MediaPlayback::Attributes::CurrentState::Id, ATTRIBUTE_FLAG_MANAGED_INTERNALLY,
                      esp_matter_enum8(0));

    command::create(cluster, Commands::Play::Id, COMMAND_FLAG_ACCEPTED,
                    dispatch<Commands::Play::DecodableType, emberAfMediaPlaybackClusterPlayCallback>);
    command::create(cluster, Commands::Pause::Id, COMMAND_FLAG_ACCEPTED,
                    dispatch<Commands::Pause::DecodableType, emberAfMediaPlaybackClusterPauseCallback>);
    command::create(cluster, Commands::Stop::Id, COMMAND_FLAG_ACCEPTED,
                    dispatch<Commands::Stop::DecodableType, emberAfMediaPlaybackClusterStopCallback>);
    command::create(cluster, Commands::Previous::Id, COMMAND_FLAG_ACCEPTED,
                    dispatch<Commands::Previous::DecodableType, emberAfMediaPlaybackClusterPreviousCallback>);
    command::create(cluster, Commands::Next::Id, COMMAND_FLAG_ACCEPTED,
                    dispatch<Commands::Next::DecodableType, emberAfMediaPlaybackClusterNextCallback>);
    command::create(cluster, Commands::Rewind::Id, COMMAND_FLAG_ACCEPTED,
                    dispatch<Commands::Rewind::DecodableType, emberAfMediaPlaybackClusterRewindCallback>);
    command::create(cluster, Commands::FastForward::Id, COMMAND_FLAG_ACCEPTED,
                    dispatch<Commands::FastForward::DecodableType, emberAfMediaPlaybackClusterFastForwardCallback>);
    command::create(cluster, Commands::PlaybackResponse::Id, COMMAND_FLAG_GENERATED, nullptr);
}

static void create_media_input(endpoint_t *endpoint)
{
    namespace Commands = MediaInput::Commands;
    cluster_t *cluster = cluster::create(endpoint, MediaInput::Id, CLUSTER_FLAG_SERVER);
    cluster::set_plugin_server_init_callback(cluster, MatterMediaInputPluginServerInitCallback);
    cluster::set_delegate_and_init_callback(
        cluster,
        [](void *delegate, uint16_t endpoint_id) {
            MediaInput::SetDefaultDelegate(endpoint_id, static_cast<MediaInput::Delegate *>(delegate));
        },
        cec_tv::media_input());
    cluster::global::attribute::create_feature_map(cluster, to_underlying(MediaInput::Feature::kNameUpdates));
    cluster::global::attribute::create_cluster_revision(cluster, MediaInput::kRevision);
    attribute::create(cluster, MediaInput::Attributes::InputList::Id, ATTRIBUTE_FLAG_MANAGED_INTERNALLY,
                      esp_matter_array(nullptr, 0, 0));
    attribute::create(cluster, MediaInput::Attributes::CurrentInput::Id, ATTRIBUTE_FLAG_MANAGED_INTERNALLY,
                      esp_matter_uint8(0));

    command::create(cluster, Commands::SelectInput::Id, COMMAND_FLAG_ACCEPTED,
                    dispatch<Commands::SelectInput::DecodableType, emberAfMediaInputClusterSelectInputCallback>);
    command::create(cluster, Commands::ShowInputStatus::Id, COMMAND_FLAG_ACCEPTED,
                    dispatch<Commands::ShowInputStatus::DecodableType, emberAfMediaInputClusterShowInputStatusCallback>);
    command::create(cluster, Commands::HideInputStatus::Id, COMMAND_FLAG_ACCEPTED,
                    dispatch<Commands::HideInputStatus::DecodableType, emberAfMediaInputClusterHideInputStatusCallback>);
    command::create(cluster, Commands::RenameInput::Id, COMMAND_FLAG_ACCEPTED,
                    dispatch<Commands::RenameInput::DecodableType, emberAfMediaInputClusterRenameInputCallback>);
}

static void create_keypad_input(endpoint_t *endpoint)
{
    cluster::keypad_input::config_t config;
    config.delegate = cec_tv::keypad_input();
    cluster_t *cluster = cluster::keypad_input::create(endpoint, &config, CLUSTER_FLAG_SERVER);
    // The SDK's server reports the feature map from the delegate
    cluster::set_plugin_server_init_callback(cluster, MatterKeypadInputPluginServerInitCallback);
    cluster::keypad_input::feature::navigation_key_codes::add(cluster);
    cluster::keypad_input::feature::location_keys::add(cluster);
    cluster::keypad_input::feature::number_keys::add(cluster);
}

static void create_low_power(endpoint_t *endpoint)
{
    cluster_t *cluster = cluster::create(endpoint, LowPower::Id, CLUSTER_FLAG_SERVER);
    cluster::set_delegate_and_init_callback(
        cluster,
        [](void *delegate, uint16_t endpoint_id) {
            LowPower::SetDefaultDelegate(endpoint_id, static_cast<LowPower::Delegate *>(delegate));
        },
        cec_tv::low_power());
    cluster::global::attribute::create_feature_map(cluster, 0);
    cluster::global::attribute::create_cluster_revision(cluster, LowPower::kRevision);
    command::create(cluster, LowPower::Commands::Sleep::Id, COMMAND_FLAG_ACCEPTED,
                    dispatch<LowPower::Commands::Sleep::DecodableType, emberAfLowPowerClusterSleepCallback>);
}

static endpoint_t *create_video_player(node_t *node)
{
    endpoint_t *endpoint = endpoint::create(node, ENDPOINT_FLAG_NONE, nullptr);
    if (!endpoint) {
        return nullptr;
    }
    endpoint::add_device_type(endpoint, k_basic_video_player_device_type_id, k_basic_video_player_device_type_version);

    cluster::descriptor::config_t descriptor_config;
    cluster::descriptor::create(endpoint, &descriptor_config, CLUSTER_FLAG_SERVER);

    cluster::on_off::config_t on_off_config;
    cluster_t *on_off = cluster::on_off::create(endpoint, &on_off_config, CLUSTER_FLAG_SERVER);
    cluster::on_off::command::create_on(on_off);
    cluster::on_off::command::create_toggle(on_off);

    create_media_playback(endpoint);
    create_media_input(endpoint);
    create_keypad_input(endpoint);
    create_low_power(endpoint);
    return endpoint;
}

// --- OTA ROLLBACK ---
// With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE an image installed by OTA boots as "pending
// verify". Keep it once it has run for a minute and is on the network (so it can still
// receive the next update); otherwise go back to the previous image.
static esp_timer_handle_t s_ota_verify_timer;

static bool app_network_up()
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    if (chip::DeviceLayer::ConnectivityMgr().IsThreadAttached()) return true;
#endif
#if CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
    if (chip::DeviceLayer::ConnectivityMgr().IsWiFiStationConnected()) return true;
#endif
    return false;
}

static void app_ota_verify_check(intptr_t)
{
    int64_t uptime_s = esp_timer_get_time() / 1000000;
    if (uptime_s >= 60 && app_network_up()) {
        esp_timer_stop(s_ota_verify_timer);
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA: new firmware confirmed");
    } else if (uptime_s >= 300) {
        ESP_LOGE(TAG, "OTA: new firmware not on the network after 5 min, rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

static void app_ota_verify_tick(void *)
{
    chip::DeviceLayer::PlatformMgr().ScheduleWork(app_ota_verify_check, 0);   // CHIP thread, stack locked
}

static void app_ota_verify_start()
{
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGW(TAG, "OTA: running new firmware, confirming once it is on the network");
    esp_timer_create_args_t args = {};
    args.callback = app_ota_verify_tick;
    args.name = "ota_verify";
    esp_timer_create(&args, &s_ota_verify_timer);
    esp_timer_start_periodic(s_ota_verify_timer, 10 * 1000000);
}

extern "C" void app_main()
{
    nvs_flash_init();

    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    endpoint_t *video_player = create_video_player(node);
    ABORT_APP_ON_FAILURE(video_player != nullptr, ESP_LOGE(TAG, "Failed to create video player endpoint"));
    s_video_player_endpoint_id = endpoint::get_id(video_player);
    cec_tv::init(s_video_player_endpoint_id);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    esp_openthread_platform_config_t config = {
        .radio_config = { .radio_mode = RADIO_MODE_NATIVE, },
        .host_config = { .host_connection_mode = HOST_CONNECTION_MODE_NONE, },
        .port_config = { .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10, },
    };
    set_openthread_platform_config(&config);
#endif

    esp_err_t err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));
    app_ota_verify_start();

    cec_tv::start();

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::factoryreset_register_commands();
    cec_tv::register_console_commands();
    esp_matter::console::attribute_register_commands();
    esp_matter::console::init();
#endif
}
