# ESP32-C5 HomeKit HDMI-CEC TV Controller

## ✨ Features

* **Native HomeKit TV Integration:** Appears as a Television accessory in the Apple Home app.
* **Power Control:** Turn your TV on and off directly from your Apple devices.
* **Input Switching:** Easily switch between HDMI 1, HDMI 2, and HDMI 3.
* **Smart Command Queuing:** Implements a background queue for input switching (800ms delay) to prevent crashing the CEC bus when tapping inputs rapidly in the Home app.
* **Control Center Remote:** Currently not working yet.
* **Volume Control:** Maps the physical volume buttons on your iOS device (when the remote widget is open) to CEC volume commands (should work but cannot test on my devices).

## 🛠 Hardware Setup
See the [hardware setup](../README.md#-hardware-setup) in the main README: an SLWF-08 with its ESP-12F swapped for a DOIT ESPC5-12, CEC on **GPIO 3**.

**Wi-Fi:** The firmware connects on **5 GHz only**. Remove the `WiFi.setBandMode()` line in `setup()` to allow 2.4 GHz as well.

## 💻 Software Dependencies

To compile and upload this code using the Arduino IDE, you will need the following libraries:

1. **HomeSpan:** The core Apple HomeKit framework for ESP32.
   * *Install via Arduino Library Manager or [HomeSpan GitHub](https://github.com/HomeSpan/HomeSpan).*
2. **CEC by s-moch:** A lightweight HDMI-CEC library for Arduino.
   * *Download and install manually from: [https://github.com/s-moch/CEC](https://github.com/s-moch/CEC).*

## 🚀 Installation & Flashing

1. Set up your Arduino IDE with the ESP32 Arduino core **3.3.0 or newer** (needed for the ESP32-C5 and 5 GHz band selection).
2. Install the required dependencies listed above.
3. Open `homespan.ino` from this folder.
4. Select **ESP32C5 Dev Module**, Flash Size **4MB**, Partition Scheme **Minimal SPIFFS (1.9MB APP with OTA)**, USB CDC On Boot **Disabled** (serial output goes to the TX/RX pads).
5. Compile and upload the code to your board.
6. Open the Serial Monitor at `115200` baud.
7. Follow the standard [HomeSpan provisioning process](https://github.com/HomeSpan/HomeSpan/blob/master/docs/CLI.md) by typing `W` in the Serial Monitor to configure your Wi-Fi.
8. Scan the HomeSpan setup code (default: `466-37-726`) in the Apple Home app to pair your new TV accessory!

## ⚙️ Customization

If your TV uses different logical or physical addresses for its inputs, you can easily modify the code:
* Search for the `pendingInput` execution block in the `loop()` of `HomeSpanTV` to change the transmitted CEC bytes for HDMI 1, 2, and 3.
* Update the `reportedInput` parsing logic in `HandleFrame` if your TV reports different physical addresses (currently looks for `0x1000`, `0x2000`, `0x3000`).
* Change the default TV name by editing `HomeSpanTV* tv = (new HomeSpanTV("Samsung TV"));`.

## 📝 License & Credits

* Apple HomeKit support is powered by [HomeSpan](https://github.com/HomeSpan/HomeSpan).
* HDMI-CEC communication is handled by [s-moch/CEC](https://github.com/s-moch/CEC).
