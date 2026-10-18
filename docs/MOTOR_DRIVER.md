# L829 / L298N Motor Driver Hardware & Integration Guide

This guide details the hardware specifications, wiring connections, power considerations, and software API for controlling dual DC motors using the **L829 / L298N Dual H-Bridge Motor Driver** on the **FlowPilot-esp32s3** robotics platform.

---

## 1. Hardware Overview

The **L298N** (commonly referenced or mistyped as **L829**) is a high-power dual full-bridge driver designed to drive inductive loads such as relays, solenoids, DC motors, and bipolar stepper motors.

### Key Specifications
- **Driver Chip**: L298N Dual H-Bridge IC
- **Motor Supply Voltage ($V_S$)**: +5V to +35V (typically 6V–12V for DC gearmotors)
- **Peak Current ($I_O$)**: Up to 2.0A per channel
- **Logic Voltage ($V_{SS}$)**: +5V (provided by internal 5V regulator if $V_S \le 12V$)
- **Control Signal Voltage**: 3.3V / 5V TTL compatible (direct ESP32-S3 3.3V logic compatible)
- **Control Modes**:
  - **3-Wire Mode (Recommended)**: Direction pins (IN1/IN2 and IN3/IN4) + Speed Enable PWM pins (ENA and ENB).
  - **2-Wire Mode**: ENA/ENB jumpers installed to 5V; PWM applied directly to direction pins.

---

## 2. Wiring & Pinout Diagram

The default pin configuration on the ESP32-S3 has been selected to prevent any conflicts with the ILI9341 SPI Display, XPT2046 Touch Controller, Octal SPI Flash/PSRAM, and onboard RGB LED.

### Pinout Mapping

| L298N / L829 Pin | ESP32-S3 GPIO | Function | Description |
| :--- | :--- | :--- | :--- |
| **ENA** | **GPIO 17** | Motor A Speed PWM | LEDC Low-Speed Channel 1 (Ultrasonic 20 kHz) |
| **IN1** | **GPIO 15** | Motor A Direction 1 | Digital GPIO Output |
| **IN2** | **GPIO 16** | Motor A Direction 2 | Digital GPIO Output |
| **IN3** | **GPIO 18** | Motor B Direction 1 | Digital GPIO Output |
| **IN4** | **GPIO 21** | Motor B Direction 2 | Digital GPIO Output |
| **ENB** | **GPIO 38** | Motor B Speed PWM | LEDC Low-Speed Channel 2 (Ultrasonic 20 kHz) |
| **GND** | **GND** | Common Ground | **Must be tied to ESP32 Ground** |
| **+12V / $V_S$** | External Battery (+) | Motor Power | 7.4V (2S LiPo) or 11.1V (3S LiPo) |
| **+5V** | 5V Regulator Output | Logic Power | Leave unconnected if ESP32 is powered via USB |

```
                +----------------------------+
                |    L298N / L829 Module     |
                |                            |
  [Motor A +]---| OUT1                  OUT3 |--[Motor B +]
  [Motor A -]---| OUT2                  OUT4 |--[Motor B -]
                |                            |
                | ENA  IN1  IN2  IN3  IN4 ENB|
                +--+----+----+----+----+---+--+
                   |    |    |    |    |   |
                   |    |    |    |    |   +------> ESP32-S3 GPIO 38 (Right PWM)
                   |    |    |    |    +---------> ESP32-S3 GPIO 21 (Right DIR 2)
                   |    |    |    +--------------> ESP32-S3 GPIO 18 (Right DIR 1)
                   |    |    +-------------------> ESP32-S3 GPIO 16 (Left DIR 2)
                   |    +------------------------> ESP32-S3 GPIO 15 (Left DIR 1)
                   +-----------------------------> ESP32-S3 GPIO 17 (Left PWM)

           Power Terminals:
           [ +12V ] ---> Battery Positive (+) [7.4V - 12V]
           [  GND ] ---> Common Ground with ESP32-S3 & Battery (-)
           [  +5V ] ---> (Optional 5V logic out if onboard regulator jumper is fitted)
```

---

## 3. Power Supply & Grounding Guidelines

> [!CAUTION]
> **Never power DC motors directly from the ESP32 development board's 3.3V or 5V rails.** DC motors draw significant startup current and induce inductive voltage spikes that can permanently damage the ESP32-S3 SoC or trigger brownout resets.

1. **Common Ground**: Ensure a solid, low-resistance ground wire connects the battery negative terminal, the L298N module GND, and the ESP32-S3 GND.
2. **Onboard 5V Regulator Jumper**:
   - If motor supply is **$\le 12V$**: Keep the 5V regulator jumper **ON**. The module generates its own internal 5V logic voltage.
   - If motor supply is **$> 12V$**: Remove the jumper and supply external +5V to the +5V terminal to prevent overheating the onboard linear regulator.
3. **Decoupling Capacitors**: Place a 100µF–470µF electrolytic capacitor across the motor power terminals ($V_S$ and GND) to buffer surge currents during rapid motor acceleration.

---

## 4. Software Architecture & API Reference

The driver is exposed via the `MotorDriver` class (also aliased as `L829Driver` and `L298NDriver`).

### 1. Header Inclusion
```cpp
#include "MotorDriver.hpp"
// Or: #include "L829Driver.hpp"
// Or: #include "L298NDriver.hpp"
```

### 2. Initialization
```cpp
// Using global default instance with preconfigured ESP32-S3 safe pins:
MotorDriver& motor = getMotorDriver();
esp_err_t err = motor.init();
if (err != ESP_OK) {
    ESP_LOGE("APP", "Motor driver init failed: %s", esp_err_to_name(err));
}
```

### 3. Custom Pin Configuration
If your hardware uses different pins:
```cpp
MotorDriverConfig cfg;
cfg.in1Pin = GPIO_NUM_1;
cfg.in2Pin = GPIO_NUM_2;
cfg.enaPin = GPIO_NUM_3;
cfg.in3Pin = GPIO_NUM_4;
cfg.in4Pin = GPIO_NUM_5;
cfg.enbPin = GPIO_NUM_6;
cfg.invertLeft = false;    // Set true if left motor is mounted reversed
cfg.invertRight = true;    // Set true if right motor is mounted reversed
cfg.slewRate = 2.5f;       // Acceleration ramp (units/sec)

MotorDriver customDriver(cfg);
customDriver.init();
```

### 4. Differential Drive Kinematics
```cpp
// Arcade Drive: drive(throttle, steering)
// throttle: [-1.0 .. 1.0] (-1 = reverse, +1 = forward)
// steering: [-1.0 .. 1.0] (-1 = full left spin, +1 = full right spin)

motor.drive(1.0f, 0.0f);   // Full speed forward
motor.drive(0.5f, -0.3f);  // Half speed forward, gentle left curve
motor.drive(-0.6f, 0.0f);  // 60% speed reverse
```

### 5. Movement Primitives
```cpp
motor.forward(0.8f);       // Forward at 80% speed
motor.backward(0.5f);      // Reverse at 50% speed
motor.turnLeft(0.7f);      // Arc turn left
motor.turnRight(0.7f);     // Arc turn right
motor.spinLeft(0.6f);      // Spin in-place counter-clockwise
motor.spinRight(0.6f);     // Spin in-place clockwise
motor.stop(true);          // Active electric brake (short brake)
motor.stop(false);         // Free-wheeling coast stop
```

### 6. Slew-Rate Ramping & Soft Start
To avoid brownouts when motors start abruptly under load, `MotorDriver` includes continuous slew-rate limiting:
- Call `motor.update(dt)` in your 60 FPS animation/control task (already integrated in `robot_face_task`).
- Adjust responsiveness:
  ```cpp
  motor.setSlewRate(4.0f); // Fast acceleration (0 to 100% in 250ms)
  motor.setSlewRate(1.5f); // Gentle, high-inertia acceleration (0 to 100% in 667ms)
  ```

---

## 5. Troubleshooting & Diagnostics

| Symptom | Probable Cause | Recommended Solution |
| :--- | :--- | :--- |
| **Motors do not spin at all** | ENA/ENB jumpers installed or removed without matching software mode | In default mode, remove ENA/ENB jumpers so ESP32 can feed PWM to them. If jumpers remain installed, switch config to `MotorControlMode::PwmDirectPins`. |
| **ESP32 reboots when motors start** | Motor inrush current causes power-rail dip (brownout) | 1. Ensure separate battery power for motors.<br>2. Add 220µF–470µF bulk capacitor across L298N $V_S$ and GND.<br>3. Lower slew rate via `motor.setSlewRate(2.0f)`. |
| **Robot steers in the wrong direction** | Motor wires inverted or mirrored orientation | Call `motor.setInverted(true, false)` or swap OUT1/OUT2 polarity. |
| **High-pitched audible motor whine** | PWM switching frequency too low | The driver defaults to **20,000 Hz (ultrasonic)**. Verify timer clock configuration has not been modified. |
