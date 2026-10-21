# FlowPilot ESP32-S3

<div align="center">

**An Intelligent Companion Robot Firmware for the ESP32-S3**  
Featuring a 60 FPS Interactive Emotional Display, Dual H-Bridge Motor Kinematics, Real-Time Hardware Telemetry, and Synchronized RGB Lighting.

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1%20%7C%20v5.x-blue.svg)](https://docs.espressif.com/projects/esp-idf/)
[![Target](https://img.shields.io/badge/Target-ESP32--S3-red.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Display](https://img.shields.io/badge/Display-ILI9341%20%2B%20XPT2046-green.svg)](https://github.com/lovyan03/LovyanGFX)
[![Motor Driver](https://img.shields.io/badge/Motor%20Driver-L829%20%2F%20L298N-orange.svg)](docs/MOTOR_DRIVER.md)
[![License](https://img.shields.io/badge/License-MIT-lightgrey.svg)](LICENSE)

</div>

---

## 🌟 Highlights & Features

### 🤖 1. Expressive 60 FPS Robot Face (`RobotFace`)
- **Graphics Pipeline**: Accelerated by [LovyanGFX](https://github.com/lovyan03/LovyanGFX) with 16-bit double-buffered DMA rendering onto a 320x240 ILI9341 LCD.
- **Procedural Kinematics**: Smooth organic eye saccades, procedural blinking, pupil dilation, and breathing rhythm.
- **Emotion State Machine**: Supports fluid transitions between multiple emotions:
  - `Dasara` *(Festive Happy Dasara Edition with sacred Chandan/Kumkum Tilak, twin flickering Diya lamps, celebratory fireworks sparkles, and Saffron-Gold Diya WS2812 LED pulse)*, `Neutral`, `Happy`, `Curious`, `Surprised`, `Focused`, `Sleepy`, `Wink`, `Sad`, and `Rage`.
- **Interactive Touch Tracking**: Integrated with the XPT2046 resistive touch controller:
  - **Gaze Tracking**: Eyes follow the user's finger in real-time across the screen.
  - **Tap Interaction**: Quick taps (<350ms) cycle between emotional states.
- **HUD Telemetry Overlay**: On-screen indicators for FPS, Core 1 & Core 2 CPU usage, free heap RAM, and SD card status.

### 🏎️ 2. L829 / L298N Dual H-Bridge Motor Driver (`MotorDriver`)
- **Silent Ultrasonic PWM**: 20 kHz hardware PWM via the ESP32-S3 LEDC peripheral with 10-bit resolution (0–1023), completely eliminating high-pitched motor whine.
- **Arcade Differential Drive Kinematics**:
  - Longitudinal throttle and rotational steering mixing (`drive(throttle, steering)`).
  - Proportional heading preservation to prevent spinout at high throttle.
- **Brownout Protection & Slew Rate Limiting**: Continuous soft-start acceleration ramps prevent voltage dips and brownout resets when DC motors switch on under load.
- **Deadzone Compensation**: Automatically overcomes static gearbox friction.
- **Dual Operating Modes**:
  - **3-Wire Mode**: ENA/ENB receive PWM, IN1–IN4 control direction.
  - **2-Wire Mode**: Direct PWM on IN1–IN4 with factory ENA/ENB jumpers installed.
- **Active Braking & Coasting**: Choose between short-circuit dynamic electrical braking and free-wheeling coast.

### 💡 3. Synchronized WS2812 Addressable RGB LED (`RgbLed`)
- Controls the onboard WS2812 addressable RGB LED (NeoPixel) on **GPIO 48**.
- **Hardware Safety Limit**: Global brightness is strictly capped to 20% (`MAX_BRIGHTNESS_LIMIT = 51` / 255 = 20.0%) to protect eyes from blinding glare and prevent brownouts.
- **Emotion Sync Mode**: Synchronizes color palette and breathing frequency directly with the active face emotion and breath cycle.
- **Standalone Modes**: Solid color, breathing/pulsing, continuous rainbow cycle, and emergency strobe.

### ⚡ 4. Dual-Core Asynchronous Architecture
- **Core 1**: Dedicated graphics rasterizer and SPI display pipeline (`robot_render_task`) delivering smooth DMA double-buffered rendering and interactive touch input.
- **Core 0**: High-speed robot kinematics supervisor, autonomous AI behavior state machines, motor slew-rate controller, LED synchronization, and performance monitor telemetry (`robot_control_task`).

---

## 📌 Master Hardware Pinout

The GPIO assignment is carefully mapped to avoid conflicts with Octal PSRAM/Flash (`GPIO 26–32`), USB JTAG (`GPIO 19, 20`), and UART0 (`GPIO 43, 44`).

| Function | Pin Name | ESP32-S3 GPIO | Bus / Interface | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **Display** | LCD MOSI | **GPIO 11** | SPI2 (FSPI) | Shared SPI Bus |
| | LCD MISO | **GPIO 13** | SPI2 (FSPI) | Shared SPI Bus |
| | LCD SCLK | **GPIO 12** | SPI2 (FSPI) | Shared SPI Bus (40 MHz) |
| | LCD CS | **GPIO 10** | SPI Chip Select | Active LOW |
| | LCD DC | **GPIO 9** | Data / Command | |
| | LCD RST | **GPIO 14** | Reset | Active LOW |
| | LCD BL | **GPIO 7** | PWM Backlight | LEDC Channel 0 (20 kHz) |
| **Touch** | Touch CS | **GPIO 5** | SPI Chip Select | Active LOW (1 MHz) |
| | Touch INT | **GPIO 6** | Interrupt | Pen-down IRQ |
| **Motor Driver** | **IN1** (Motor A PWM 1) | **GPIO 15** | LEDC Channel 1 | Left Motor Direct PWM (20 kHz) |
| **(L829 / L298N)** | **IN2** (Motor A PWM 2) | **GPIO 16** | LEDC Channel 2 | Left Motor Direct PWM (20 kHz) |
| | **IN3** (Motor B PWM 1) | **GPIO 18** | LEDC Channel 3 | Right Motor Direct PWM (20 kHz) |
| | **IN4** (Motor B PWM 2) | **GPIO 17** | LEDC Channel 4 | Right Motor Direct PWM (20 kHz) |
| | **ENA / ENB** | *None* | Onboard 5V Jumpers | **Excluded** (Jumpers left ON 5V) |
| **Onboard RGB** | WS2812 DIN | **GPIO 48** | RMT / SPI Strip | Onboard DevKit LED |

> [!TIP]
> **ENA and ENB pins are excluded by default.** By keeping the factory 5V jumper caps on the L298N board, speed and direction are controlled directly via 4 PWM lines on IN1–IN4 (GPIOs 15, 16, 17, 18), forming a convenient contiguous block on the dev board. For full wiring diagrams, see the **[Motor Driver Hardware Guide](docs/MOTOR_DRIVER.md)**.

---

## 📂 Project Structure

```
FlowPilot-esp32s3/
├── CMakeLists.txt              # Root CMake project configuration
├── README.md                   # Primary project documentation
├── sdkconfig.defaults          # ESP32-S3 default configs (PSRAM, Flash, FreeRTOS)
├── docs/
│   └── MOTOR_DRIVER.md         # Dedicated L829 / L298N wiring & integration guide
├── components/
│   └── LovyanGFX/              # Ultra-fast display driver component
├── main/
│   ├── CMakeLists.txt          # Main component build rules and dependencies
│   ├── idf_component.yml       # ESP-IDF component dependencies
│   ├── main.cpp                # app_main & dual-core task orchestrator
│   ├── LGFX_Config.hpp         # LovyanGFX panel, bus, backlight & touch setup
│   ├── RobotFace.hpp / .cpp    # 60 FPS procedural emotional face engine
│   ├── MotorDriver.hpp / .cpp  # L829 / L298N dual H-bridge motor driver
│   ├── L829Driver.hpp          # Forwarding header for L829 naming
│   ├── L298NDriver.hpp         # Forwarding header for L298N naming
│   └── RgbLed.hpp / .cpp       # WS2812 emotion-synced RGB LED controller
```

---

## 🚀 Quick Start & Building

### Prerequisites
- **ESP-IDF**: Version 5.1 or newer (tested on **ESP-IDF v6.1**).
- **Target Hardware**: ESP32-S3 development board (16MB Flash QIO, 8MB Octal PSRAM).
- **Display**: 2.8" or 3.2" SPI TFT ILI9341 (320x240) with XPT2046 touch controller.
- **Motor Driver**: L829 / L298N dual H-bridge module with 2 DC gearmotors.

### 1. Set Up Environment
```bash
# Source the ESP-IDF environment
. $HOME/esp/esp-idf/export.sh
# (or your local ESP-IDF path, e.g. . ~/.espressif/v6.1/esp-idf/export.sh)
```

### 2. Set Target & Build
```bash
# Set target chip to ESP32-S3
idf.py set-target esp32s3

# Compile the firmware
idf.py build
```

### 3. Flash & Monitor
```bash
# Flash firmware and launch serial console (replace /dev/ttyACM0 with your serial port)
idf.py -p /dev/ttyACM0 flash monitor
```

*Press `Ctrl + ]` to exit the serial monitor.*

---

## 💻 Code Usage Examples

### Controlling Motors via Differential Drive
```cpp
#include "MotorDriver.hpp"

// Access the global MotorDriver instance
MotorDriver& motors = getMotorDriver();

// Arcade drive: drive(throttle, steering)
motors.drive(1.0f, 0.0f);    // Drive straight forward at 100% speed
motors.drive(-0.5f, 0.0f);   // Drive backward at 50% speed
motors.drive(0.8f, 0.4f);    // Forward with a right turn
motors.spinLeft(0.7f);       // Spin in place counter-clockwise

// Stop options
motors.stop(true);           // Active electrical short-brake (immediate stop)
motors.stop(false);          // Free-wheeling coast to a halt
```

### Cycling Robot Face Emotions
```cpp
#include "RobotFace.hpp"

// Set specific emotional state
robotFace.setEmotion(RobotEmotion::Happy);
robotFace.setEmotion(RobotEmotion::Excited);

// Or advance to the next emotion
robotFace.nextEmotion();
```

### Customizing Motor Pins & Dynamics
```cpp
MotorDriverConfig cfg;
cfg.in1Pin = GPIO_NUM_1;
cfg.in2Pin = GPIO_NUM_2;
cfg.enaPin = GPIO_NUM_3;
cfg.slewRate = 2.0f;        // Gentle acceleration (prevents high inrush currents)
cfg.minSpeed = 0.20f;       // Minimum 20% speed to overcome gearbox friction
cfg.invertLeft = false;
cfg.invertRight = true;     // Flip right motor if mounted 180 degrees reversed

getMotorDriver().setConfig(cfg);
```

---

## 🔧 Power & Troubleshooting

- **Motor Supply vs ESP32 Supply**: Always power DC motors through an external battery pack (7.4V–12V). **Connect all grounds (ESP32 GND, Battery GND, L298N GND) together.**
- **Brownout Resets**: Sudden high-speed motor transitions can pull battery voltage down. Slew rate ramping is enabled by default (`config.enableRamping = true`) to smoothly ramp power.
- **Touch Calibration**: Touch screen calibration coordinates can be fine-tuned in [main/LGFX_Config.hpp](main/LGFX_Config.hpp) (`x_min`, `x_max`, `y_min`, `y_max`).

---

## 📜 License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
