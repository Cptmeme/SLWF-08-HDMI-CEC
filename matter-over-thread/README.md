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

## Status

Builds; not yet tested on hardware. Home Assistant support for Matter video players is in
review ([home-assistant/core#186532](https://github.com/home-assistant/core/pull/186532)).
