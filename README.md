# SLWF-08 HDMI-CEC TV Controller (ESP32-C5)

Turns an SMLight SLWF-08 HDMI-CEC board into a smart-home TV controller: power, input switching and remote keys over HDMI-CEC.

| Folder | Shows up as | Network |
|---|---|---|
| [`homespan/`](homespan) | HomeKit Television accessory in Apple Home ([HomeSpan](https://github.com/HomeSpan/HomeSpan), Arduino) | Wi-Fi, 5 GHz |
| [`matter-over-thread/`](matter-over-thread) | Matter Basic Video Player, e.g. a media player in Home Assistant ([esp-matter](https://github.com/espressif/esp-matter), ESP-IDF) | Thread |

## 🛠 Hardware Setup
Both versions run on a modified **SMLight SLWF-08** HDMI-CEC controller.

1. **Desolder** the factory ESP-12F (ESP8266) module from the SMLight board.
2. **Solder** a DOIT `ESPC5-12` (ESP32-C5) module in its place. It shares the ESP-12F footprint, and the ESP32-C5 adds 5 GHz Wi-Fi.
3. **CEC Line:** The SLWF-08 routes CEC to the ESP-12F `IO14` pad (pin 5), which is **GPIO 3** on the ESPC5-12.

ESP-12F pad → ESPC5-12 GPIO, for reference:

| ESP-12F pad | ESPC5-12 | | ESP-12F pad | ESPC5-12 |
|---|---|---|---|---|
| 1 RST | GPIO0 | | 22 TXD | TX0 (GPIO11) |
| 2 ADC | GPIO1 | | 21 RXD | RX0 (GPIO12) |
| 3 EN | EN | | 20 IO5 | GPIO26 (strapping) |
| 4 IO16 | GPIO2 | | 19 IO4 | GPIO27 (strapping) |
| 5 IO14 | **GPIO3 (CEC)** | | 18 IO0 | GPIO28 (boot) |
| 6 IO12 | GPIO4 | | 17 IO2 | GPIO15 |
| 7 IO13 | GPIO5 | | 16 IO15 | GPIO6 |
| 8 VCC | VCC | | 15 GND | GND |

## 📝 License

GPL-3.0, see [LICENSE](LICENSE).
