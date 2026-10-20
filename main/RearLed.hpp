#pragma once

#include <cstdint>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

// Forward declarations to maintain loose coupling
class MotorDriver;
class RobotFace;
enum class RobotEmotion;

/**
 * @brief Operational and lighting modes for the Rear Blue LED
 */
enum class RearLedMode {
    Off,          ///< LED turned off
    Solid,        ///< Constant steady illumination (taillight / night running light)
    Breathing,    ///< Smooth sinusoidal breathing / pulsing vitality glow
    Blink,        ///< Periodic beacon flashing / strobe
    Heartbeat,    ///< Double-pulse organic companion heartbeat
    ReverseSync,  ///< Autonomous vehicle mode: gentle taillight when idling/forward, bright warning strobe when reversing
    EmotionSync   ///< Dynamically synchronized with RobotFace emotions & breath rhythm
};

/**
 * @brief High-resolution hardware PWM controller for the Rear Blue LED on ESP32-S3
 *
 * Hardware Mapping:
 * - Assigned GPIO: GPIO_NUM_21 (Exposed on ESP32-S3 DevKit-C pin header J3)
 * - Hardware Peripheral: LEDC Timer 1, Channel 5 (5 kHz flicker-free 10-bit PWM)
 * - Safe from conflicts with SPI display (4..14), Motor driver (15..18), RGB NeoPixel (48),
 *   Octal Flash/PSRAM (26..37), and USB/JTAG (19, 20).
 */
class RearLed {
public:
    /// Default GPIO pin for the rear blue LED (GPIO 21 on ESP32-S3 DevKit)
    static constexpr gpio_num_t DEFAULT_GPIO_PIN = GPIO_NUM_21;

    /// Default LEDC channel (Channel 5; 0=Backlight, 1..4=Motors)
    static constexpr ledc_channel_t DEFAULT_LEDC_CHANNEL = LEDC_CHANNEL_5;

    /// Default LEDC timer (Timer 1; Timer 0 is used by MotorDriver)
    static constexpr ledc_timer_t DEFAULT_LEDC_TIMER = LEDC_TIMER_1;

    /// Default PWM frequency (5 kHz provides completely flicker-free and silent operation)
    static constexpr uint32_t DEFAULT_PWM_FREQ_HZ = 5000;

    /// Default PWM resolution (10-bit: 0 to 1023 duty steps)
    static constexpr ledc_timer_bit_t DEFAULT_PWM_RESOLUTION = LEDC_TIMER_10_BIT;

    /// Default user brightness level [0..255] (Full 100% brightness: 255/255, unrestricted)
    static constexpr uint8_t DEFAULT_BRIGHTNESS = 255;

    RearLed();
    ~RearLed();

    /**
     * @brief Initialize the Rear Blue LED with hardware LEDC PWM
     * @param pin GPIO pin connected to the rear LED (defaults to GPIO 21)
     * @param channel LEDC channel (defaults to LEDC_CHANNEL_5)
     * @param timer LEDC timer (defaults to LEDC_TIMER_1)
     * @param activeHigh True if LED turns on when pin is HIGH (standard anode drive)
     * @return esp_err_t ESP_OK on success, or ESP-IDF error code
     */
    esp_err_t init(gpio_num_t pin = DEFAULT_GPIO_PIN,
                   ledc_channel_t channel = DEFAULT_LEDC_CHANNEL,
                   ledc_timer_t timer = DEFAULT_LEDC_TIMER,
                   bool activeHigh = true);

    /**
     * @brief Periodic non-blocking update for animations, breathing curves, and transitions
     * @param dt Delta time in seconds since the last frame
     */
    void update(float dt);

    /**
     * @brief Set operational lighting mode
     * @param mode Selected RearLedMode
     */
    void setMode(RearLedMode mode);
    RearLedMode getMode() const { return currentMode; }

    /**
     * @brief Turn LED fully ON (solid at current brightness)
     */
    void on();

    /**
     * @brief Turn LED fully OFF
     */
    void off();

    /**
     * @brief Toggle LED between off and previous on state
     */
    void toggle();

    /**
     * @brief Returns true if LED is emitting light
     */
    bool isOn() const { return currentDutyRaw > 0; }

    /**
     * @brief Set brightness level (0 = off, 255 = maximum brightness)
     * Applies quadratic gamma correction for human eye perception
     * @param brightness Value from 0 to 255
     */
    void setBrightness(uint8_t brightness);
    uint8_t getBrightness() const { return userBrightness; }

    /**
     * @brief Set brightness by percentage (0.0% to 100.0%)
     * @param percent Value from 0.0 to 100.0
     */
    void setBrightnessPercent(float percent);
    float getBrightnessPercent() const;

    /**
     * @brief Set blink frequency in Hz
     * @param freqHz Frequency in Hz (e.g. 2.0 = 2 blinks per second)
     */
    void setBlinkRate(float freqHz);
    float getBlinkRate() const { return blinkRateHz; }

    /**
     * @brief Set breathing cycle period in seconds
     * @param periodSec Duration in seconds of one full breathe in/out cycle
     */
    void setBreathPeriod(float periodSec);
    float getBreathPeriod() const { return breathPeriodSec; }

    /**
     * @brief Synchronize with MotorDriver: enables autonomous reverse taillight
     * @param motorDriver Reference to initialized MotorDriver instance
     */
    void syncWithMotors(const MotorDriver& motorDriver);

    /**
     * @brief Synchronize with RobotFace: matches face emotions and breathing
     * @param robotFace Reference to initialized RobotFace instance
     */
    void syncWithFace(const RobotFace& robotFace);

    /**
     * @brief Get active GPIO pin
     */
    gpio_num_t getPin() const { return gpioPin; }

    /**
     * @brief Check initialization status
     */
    bool isReady() const { return initialized; }

private:
    void applyHardwareDuty(uint32_t dutyRaw);
    uint32_t calculateDutyFromBrightness(uint8_t brightness) const;

    gpio_num_t gpioPin;
    ledc_channel_t ledcChannel;
    ledc_timer_t ledcTimer;
    ledc_mode_t speedMode;
    ledc_timer_bit_t pwmResolution;
    uint32_t maxPwmDuty;
    bool activeHigh;
    bool initialized;

    RearLedMode currentMode;
    uint8_t userBrightness;     ///< Master user brightness [0..255]
    uint32_t currentDutyRaw;    ///< Currently applied hardware duty

    // Animation & telemetry states
    float animTimer;            ///< General animation accumulator in seconds
    float blinkRateHz;          ///< Blink frequency in Hz (default 2.0 Hz)
    float breathPeriodSec;      ///< Breathing period in seconds (default 2.5 s)
    bool isReversing;           ///< Reverse flag from motor driver
    float reverseStrobeTimer;   ///< Strobe accumulator during reverse gear
    int currentEmotionInt;      ///< Cached RobotEmotion as integer for loose coupling
};

/**
 * @brief Global singleton accessor for the Rear Blue LED
 */
RearLed& getRearLed();
