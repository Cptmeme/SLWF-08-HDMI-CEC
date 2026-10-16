# HDMI-CEC TV controller (Matter over Thread)

Matter-over-Thread firmware for the SLWF-08 with an ESPC5-12 (see the
[hardware setup](../README.md#-hardware-setup)). The TV shows up as a Matter
**Basic Video Player** (device type 0x0028), controlled over HDMI-CEC:

| Matter | CEC |
|---|---|
| On/Off | `<Image View On>` / `<Standby>` to the TV; follows `<Report Power Status>` (polled every 30 s) and `<Standby>` |
| Media Playback: Play, Pause, Stop, Previous, Next, Rewind, FastForward | `<User Control Pressed>` to the source on the current input (the TV when that source is unknown) |
| Media Input: Live TV and HDMI 1-3 | selecting HDMI *n* broadcasts `<Active Source>` and `<Set Stream Path>` for *n*.0.0.0; follows `<Active Source>`, `<Routing Change>` and `<Set Stream Path>` |
| Media Input: RenameInput | names are kept in NVS |
| Keypad Input: SendKey | `<User Control Pressed>` to the TV (Matter key codes are the CEC UI command codes) |
| Low Power: Sleep | `<Standby>` to the TV |

CEC has no reliable way to read the playback state, so Media Playback's CurrentState follows
the last command. The device answers the mandatory CEC queries itself (physical address, OSD
name, CEC version, power status, vendor ID) and claims playback logical address 4, 8 or 11.

The CEC bit timing runs in a 50 µs hardware-timer interrupt that keeps running while Matter
writes to flash. It is a native ESP-IDF driver, not the Arduino CEC library the
[HomeSpan version](../homespan) uses.

## Hardware

- Flash over the UART pads (TXD/RXD = GPIO11/GPIO12), holding the IO0 pad (GPIO28) low at reset.
  The console uses the same pads.
- The board has no button: factory reset with `matter esp factoryreset` on the console.

## Configuration

`idf.py menuconfig` → **HDMI-CEC**:

- **Physical address**: the TV port the SLWF-08 is plugged into (default 0x1000 = HDMI 1).
- **Number of TV HDMI inputs** (default 3).
- **Name shown on the TV** (default "Matter").

## Building

Needs ESP-IDF v5.5.1 or newer (for the ESP32-C5) and esp-matter. See the
[docs](https://docs.espressif.com/projects/esp-matter/en/latest/esp32c5/developing.html).

```
cd matter-over-thread
idf.py set-target esp32c5
idf.py build flash monitor
```

## Updating the firmware

### Versions

Bump all three for every release, or the device ignores the update. The build fails if the two
numbers differ.

- `PROJECT_VER` (e.g. `"1.1"`) and `PROJECT_VER_NUMBER` (e.g. `2`) in `CMakeLists.txt`
- `CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER` in `sdkconfig` / `sdkconfig.defaults` (same number)

### Over the UART pads

A normal `idf.py flash` still works (IO0 low at reset, see [Hardware](#hardware)). It is also the
only way to write the bootloader, and the automatic rollback below needs the bootloader from this
build. A device that is only ever updated over the air still updates fine; it just won't roll back.

### Over the air (Matter OTA over Thread)

The build also produces `build/hdmi_cec_tv-ota.bin`, a Matter OTA image. Apple Home and Google
Home only update certified devices, so you serve it yourself with the Matter SDK's host tools.
Build them from the SDK with
`./scripts/build/build_examples.py --target darwin-arm64-chip-tool --target darwin-arm64-ota-provider build`.

One-time setup: in your controller app, turn on pairing mode for the device, then add the provider
and the device to `chip-tool`'s fabric:

```bash
chip-tool pairing code 2 <setup-code-from-home-app>              # SLWF-08 = node 2
chip-ota-provider-app -f build/hdmi_cec_tv-ota.bin --discriminator 3841 --secured-device-port 5565 &
chip-tool pairing onnetwork-long 1 20202021 3841                 # provider = node 1
chip-tool accesscontrol write acl '[{"fabricIndex": 1, "privilege": 5, "authMode": 2, "subjects": [112233], "targets": null}, {"fabricIndex": 1, "privilege": 3, "authMode": 2, "subjects": null, "targets": [{"cluster": 41, "endpoint": null, "deviceType": null}]}]' 1 0
```

Each update (with the provider running and serving the new `hdmi_cec_tv-ota.bin`):

```bash
chip-tool otasoftwareupdaterequestor announce-otaprovider 1 0 2 0 2 0
chip-tool basicinformation read software-version 2 0              # afterwards: the new number
```

Thread is slow for this: about 12 minutes per 1.5 MB, so roughly 15 minutes for this image. The
device then restarts into the new version and keeps it once it has run for a minute while attached
to the Thread network. If it doesn't get there within 5 minutes, or crashes before that, the
bootloader automatically returns to the previous version.

## Status

Builds; not yet tested on hardware. Home Assistant support for Matter video players is in
review ([home-assistant/core#186532](https://github.com/home-assistant/core/pull/186532)).
