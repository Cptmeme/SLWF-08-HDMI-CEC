#pragma once

#include <stdint.h>

#include <app/clusters/keypad-input-server/keypad-input-server.h>
#include <app/clusters/low-power-server/low-power-server.h>
#include <app/clusters/media-input-server/media-input-server.h>
#include <app/clusters/media-playback-server/media-playback-server.h>

// The TV as seen over HDMI-CEC, and the Matter delegates that control it
namespace cec_tv {

// Load the input names; call before esp_matter::start()
void init(uint16_t endpoint_id);

// Start the CEC bus and follow the TV's state; call after esp_matter::start()
void start();

// The On/Off attribute was written by a controller
void set_power(bool on);

// Add "matter esp cec" to the console
void register_console_commands();

chip::app::Clusters::MediaPlayback::Delegate *media_playback();
chip::app::Clusters::MediaInput::Delegate *media_input();
chip::app::Clusters::KeypadInput::Delegate *keypad_input();
chip::app::Clusters::LowPower::Delegate *low_power();

} // namespace cec_tv
