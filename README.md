# RC Ball-Launching Buggy

Control code for an autonomous pickleball launcher built on a gutted toy car chassis, entered in the Blueprint.io Summer Build Contest.

An ESP32 hosts its own WiFi access point, so a phone can connect and control the launcher directly with no home router or internet connection needed. Two flywheel ESCs and a gate servo are driven through a PCA9685 PWM board over I2C.

## Files

- **`flywheel_control/flywheel_control.ino`** - Main control sketch. Hosts a WiFi access point and a web control page (arm/stop, independent speed sliders for each flywheel motor, gate open/close). Includes a heartbeat watchdog that stops the motors if the connection to the phone is lost.
- **`esc_calibration/esc_calibration.ino`** - Standalone sketch for calibrating the flywheel ESCs' throttle range through the PCA9685, step-paced over Serial Monitor rather than on a timer.
- **`esc_test.py`** - Early bench-test script (Raspberry Pi / gpiozero), superseded by the ESP32 sketches above.

## Hardware

- Freenove ESP32-WROOM DevKit
- PCA9685 16-channel PWM driver (I2C)
- 2x brushless flywheel motors + ESCs (mounted on salvaged RC plane landing gear wheels)
- 1x gate servo (ball feed)
- 3S LiPo battery, physical inline kill switch

## Libraries required

- `Adafruit PWM Servo Driver Library` (Arduino Library Manager)
- Built-in `WiFi.h` / `WebServer.h` (ESP32 board package)

## Setup

1. Wire the PCA9685: ESP32 GPIO21 -> SDA, GPIO22 -> SCL, ESP32 3.3V -> VCC, shared GND. Servo power (V+) comes from a separate 5V source.
2. Flash `flywheel_control.ino`. On boot it prints its access point name and IP address to Serial Monitor (115200 baud).
3. Connect a phone to the ESP32's WiFi network and open the printed IP address in a browser.
4. Arm, then use the sliders to test each flywheel independently before combining them.

## Media

Build photos, timelapses, and test videos live in [`media/`](media/).
