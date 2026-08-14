# RC Ball-Launching Buggy

Control code for an autonomous pickleball launcher built on a gutted toy car chassis, entered in the Blueprint.io Summer Build Contest.

An ESP32 hosts its own WiFi access point, so a phone can connect and control the launcher directly with no home router or internet connection needed. Two flywheel ESCs, a drive ESC, and a gate servo are all driven through a PCA9685 PWM board over I2C. The net stores a batch of balls and feeds them one at a time through the gate rather than catching live returns.

## Files

- **`flywheel_control/flywheel_control.ino`** - Manual control sketch. Hosts a WiFi access point and a web control page: arm/stop, independent speed sliders for each flywheel motor, gate open/close, and a confirm-gated "Drive Burst" button that runs the drive ESC forward for 2 seconds then returns to neutral. Includes a heartbeat watchdog that stops the flywheel/drive motors if the connection to the phone is lost.
- **`train/train.ino`** - Demo/training sketch. The buggy is always fully stopped when it shoots: SHOOT (gate open, drive neutral) -> cooldown -> DRIVE one segment -> cooldown -> SHOOT again. "Balls per pass" splits the full net length into that many equal segments and fires one ball at the start of each; direction only flips once a full pass is complete, not after every shot. Flywheels spin up once and stay spinning through the whole run. Gate open time is fixed at 500ms in code (`GATE_OPEN_MS`), not user-adjustable - it tested fine and didn't need a slider. Main screen: balls to fire, balls per pass, flywheel power, drive power (default 60%), traverse time (how long a full end-to-end drive takes - not measured yet, needs a real bench test). Advanced Settings holds the two cooldown pauses and a **Traverse Test** button: it runs the same number of stop-start segments as "balls per pass" at the current Drive Power/Traverse Time, with no gate and no flywheels involved, so the timing can be dialed in without firing any balls. It deliberately mimics the real run's repeated stop-start motion rather than one continuous drive - a continuous test measures a shorter distance than the segmented real run actually needs, since restarting from a dead stop several times costs more than accelerating once. Serial Monitor logs each shot fired and each direction flip, useful for confirming the sequence is doing what you expect when it's hard to tell by eye. Same heartbeat safety stop as the manual sketch. Only one sketch runs on the ESP32 at a time - flash whichever one you need for that session.
- **`esc_calibration/esc_calibration.ino`** - Standalone sketch for calibrating the flywheel ESCs' throttle range through the PCA9685, step-paced over Serial Monitor rather than on a timer.

## Gate calibration

`GATE_OPEN_ANGLE` / `GATE_CLOSED_ANGLE` are set from a 2026-08-11 bench test on the ball-storage gate (20 degrees = open, 160 = closed). If the gate hardware changes, re-test and update both constants in `flywheel_control.ino` and `train.ino` together - they're independent copies, not shared.

## Channel map (PCA9685)

- 0 - gate servo
- 1 - left flywheel ESC
- 2 - right flywheel ESC
- 3 - drive ESC (QuicRun 1060, both drive motors wired in parallel)

## Hardware

- Freenove ESP32-WROOM DevKit
- PCA9685 16-channel PWM driver (I2C)
- 2x brushless flywheel motors + ESCs (mounted on salvaged RC plane landing gear wheels)
- Hobbywing QuicRun 1060 ESC driving both stock toy-car drive motors in parallel
- 1x gate servo (ball feed)
- 3S LiPo battery + physical inline kill switch, powers the bus bar and all 3 ESCs
- 5V 1A USB power bank, powers the PCA9685 (V+) and ESP32 - a separate battery from the motor rail, not a UBEC

## Libraries required

- `Adafruit PWM Servo Driver Library` (Arduino Library Manager)
- Built-in `WiFi.h` / `WebServer.h` (ESP32 board package)

## Setup

1. Wire the PCA9685: ESP32 GPIO21 -> SDA, GPIO22 -> SCL, ESP32 3.3V -> VCC, shared GND. Servo power (V+) comes from a separate 5V 1A USB power bank, not the motor battery and not a UBEC - keeping logic power off the motor rail is what stops ESC current draw from ever browning out the ESP32.
2. Flash `flywheel_control.ino`. On boot it prints its access point name and IP address to Serial Monitor (115200 baud).
3. Connect a phone to the ESP32's WiFi network and open the printed IP address in a browser.
4. Arm, then use the sliders to test each flywheel independently before combining them.
5. Note the drive ESC is bidirectional and arms at neutral (1500us), unlike the flywheel ESCs which arm at minimum (1000us) - this is handled automatically in code, just worth knowing if you're debugging with a scope/logic analyzer.

## Media

View photos, timelapses, and test videos in [`media/`](media/).
