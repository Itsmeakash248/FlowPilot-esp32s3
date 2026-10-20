#include "RearLed.hpp"
#include "MotorDriver.hpp"
#include "RobotFace.hpp"
#include "esp_log.h"
#include <algorithm>
#include <cmath>

static const char *TAG = "REAR_LED";

RearLed::RearLed()
    : gpioPin(DEFAULT_GPIO_PIN)
    , ledcChannel(DEFAULT_LEDC_CHANNEL)
    , ledcTimer(DEFAULT_LEDC_TIMER)
    , speedMode(LEDC_LOW_SPEED_MODE)
    , pwmResolution(DEFAULT_PWM_RESOLUTION)
    , maxPwmDuty((1UL << DEFAULT_PWM_RESOLUTION) - 1)
    , activeHigh(true)
    , initialized(false)
    , currentMode(RearLedMode::ReverseSync)
    , userBrightness(DEFAULT_BRIGHTNESS)
    , currentDutyRaw(0)
    , animTimer(0.0f)
    , blinkRateHz(2.0f)
    , breathPeriodSec(2.5f)
    , isReversing(false)
    , reverseStrobeTimer(0.0f)
    , currentEmotionInt(0)
{
}

RearLed::~RearLed() {
    if (initialized) {
        off();
    }
}

esp_err_t RearLed::init(gpio_num_t pin, ledc_channel_t channel, ledc_timer_t timer, bool activeHighDrive) {
    gpioPin = pin;
    ledcChannel = channel;
    ledcTimer = timer;
    activeHigh = activeHighDrive;
    maxPwmDuty = (1UL << pwmResolution) - 1;

    ESP_LOGI(TAG, "Initializing Rear Blue LED on GPIO %d (LEDC Channel %d, Timer %d, 5 kHz, 10-bit PWM)...",
             (int)gpioPin, (int)ledcChannel, (int)ledcTimer);

    // 1. Configure LEDC hardware timer
    ledc_timer_config_t timer_conf = {};
    timer_conf.speed_mode = speedMode;
    timer_conf.duty_resolution = pwmResolution;
    timer_conf.timer_num = ledcTimer;
    timer_conf.freq_hz = DEFAULT_PWM_FREQ_HZ;
    timer_conf.clk_cfg = LEDC_AUTO_CLK;

    esp_err_t err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC timer! Error: %s", esp_err_to_name(err));
        return err;
    }

    // 2. Configure LEDC hardware channel
    ledc_channel_config_t ch_conf = {};
    ch_conf.gpio_num = gpioPin;
    ch_conf.speed_mode = speedMode;
    ch_conf.channel = ledcChannel;
    ch_conf.timer_sel = ledcTimer;
    ch_conf.duty = activeHigh ? 0 : maxPwmDuty; // Start in OFF state
    ch_conf.hpoint = 0;

    err = ledc_channel_config(&ch_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC channel! Error: %s", esp_err_to_name(err));
        return err;
    }

    initialized = true;
    currentDutyRaw = 0;
    applyHardwareDuty(0);

    ESP_LOGI(TAG, "Rear Blue LED initialized successfully on GPIO %d (Active %s, Mode: %s).",
             (int)gpioPin, activeHigh ? "HIGH" : "LOW",
             currentMode == RearLedMode::ReverseSync ? "ReverseSync" : "Default");

    return ESP_OK;
}

void RearLed::applyHardwareDuty(uint32_t dutyRaw) {
    if (!initialized) return;

    if (dutyRaw > maxPwmDuty) {
        dutyRaw = maxPwmDuty;
    }

    uint32_t effectiveDuty = activeHigh ? dutyRaw : (maxPwmDuty - dutyRaw);
    currentDutyRaw = dutyRaw;

    ledc_set_duty(speedMode, ledcChannel, effectiveDuty);
    ledc_update_duty(speedMode, ledcChannel);
}

uint32_t RearLed::calculateDutyFromBrightness(uint8_t brightness) const {
    if (brightness == 0) return 0;
    // Normalized 0.0 .. 1.0 with quadratic gamma correction (~2.2) for natural eye response
    float norm = (float)brightness / 255.0f;
    float gammaDuty = norm * norm;
    return (uint32_t)std::round(gammaDuty * (float)maxPwmDuty);
}

void RearLed::setMode(RearLedMode mode) {
    currentMode = mode;
    animTimer = 0.0f;
    reverseStrobeTimer = 0.0f;

    if (mode == RearLedMode::Off) {
        applyHardwareDuty(0);
    } else if (mode == RearLedMode::Solid) {
        applyHardwareDuty(calculateDutyFromBrightness(userBrightness));
    }
}

void RearLed::on() {
    setMode(RearLedMode::Solid);
}

void RearLed::off() {
    setMode(RearLedMode::Off);
}

void RearLed::toggle() {
    if (isOn()) {
        off();
    } else {
        on();
    }
}

void RearLed::setBrightness(uint8_t brightness) {
    userBrightness = brightness;
    if (currentMode == RearLedMode::Solid) {
        applyHardwareDuty(calculateDutyFromBrightness(userBrightness));
    }
}

void RearLed::setBrightnessPercent(float percent) {
    percent = std::clamp(percent, 0.0f, 100.0f);
    setBrightness((uint8_t)std::round((percent / 100.0f) * 255.0f));
}

float RearLed::getBrightnessPercent() const {
    return ((float)userBrightness / 255.0f) * 100.0f;
}

void RearLed::setBlinkRate(float freqHz) {
    blinkRateHz = std::max(0.1f, std::min(freqHz, 50.0f));
}

void RearLed::setBreathPeriod(float periodSec) {
    breathPeriodSec = std::max(0.2f, std::min(periodSec, 30.0f));
}

void RearLed::syncWithMotors(const MotorDriver& motorDriver) {
    const MotorTelemetry& telem = motorDriver.getTelemetry();
    // Reverse detected when commanded throttle is negative or wheel speeds are reversing
    isReversing = (telem.throttle < -0.05f) ||
                  (telem.currentLeft < -0.05f && telem.currentRight < -0.05f);
}

void RearLed::syncWithFace(const RobotFace& robotFace) {
    currentEmotionInt = (int)robotFace.getEmotion();
}

void RearLed::update(float dt) {
    if (!initialized) return;

    switch (currentMode) {
    case RearLedMode::Off:
        if (currentDutyRaw != 0) {
            applyHardwareDuty(0);
        }
        break;

    case RearLedMode::Solid:
        // Already set or kept steady
        break;

    case RearLedMode::Breathing: {
        animTimer += dt;
        float phase = (animTimer / breathPeriodSec) * 2.0f * (float)M_PI;
        float sineVal = 0.5f + 0.5f * std::sin(phase);
        float breathingFactor = 0.04f + 0.96f * (sineVal * sineVal);
        uint8_t effectiveBri = (uint8_t)std::clamp((int)std::round((float)userBrightness * breathingFactor), 0, 255);
        applyHardwareDuty(calculateDutyFromBrightness(effectiveBri));
        break;
    }

    case RearLedMode::Blink: {
        animTimer += dt;
        float period = (blinkRateHz > 0.05f) ? (1.0f / blinkRateHz) : 1.0f;
        bool state = std::fmod(animTimer, period) < (period * 0.5f);
        applyHardwareDuty(state ? calculateDutyFromBrightness(userBrightness) : 0);
        break;
    }

    case RearLedMode::Heartbeat: {
        animTimer += dt;
        // 1.25 second organic heartbeat cycle: lub-DUB ... pause
        float cycleTime = std::fmod(animTimer, 1.25f);
        float pulseFactor = 0.0f;

        if (cycleTime < 0.15f) {
            pulseFactor = std::sin((cycleTime / 0.15f) * (float)M_PI) * 0.70f;
        } else if (cycleTime >= 0.22f && cycleTime < 0.42f) {
            pulseFactor = std::sin(((cycleTime - 0.22f) / 0.20f) * (float)M_PI) * 1.0f;
        } else {
            pulseFactor = 0.04f;
        }

        uint8_t effectiveBri = (uint8_t)std::clamp((int)std::round((float)userBrightness * pulseFactor), 0, 255);
        applyHardwareDuty(calculateDutyFromBrightness(effectiveBri));
        break;
    }

    case RearLedMode::ReverseSync: {
        if (isReversing) {
            // Full-blast reverse warning strobe (5 Hz rapid alert)
            reverseStrobeTimer += dt;
            bool strobeOn = std::fmod(reverseStrobeTimer, 0.20f) < 0.10f;
            uint8_t warningBri = strobeOn ? userBrightness : 0;
            applyHardwareDuty(calculateDutyFromBrightness(warningBri));
        } else {
            // Forward / Stationary running taillight at full brightness with gentle breathing
            reverseStrobeTimer = 0.0f;
            animTimer += dt;
            float idleBreath = 0.5f + 0.5f * std::sin((animTimer / 3.0f) * 2.0f * (float)M_PI);
            float taillightFactor = 0.60f + 0.40f * idleBreath; // 60% to 100% full brightness
            uint8_t taillightBri = (uint8_t)std::clamp((int)std::round((float)userBrightness * taillightFactor), 0, 255);
            applyHardwareDuty(calculateDutyFromBrightness(taillightBri));
        }
        break;
    }

    case RearLedMode::EmotionSync: {
        animTimer += dt;
        RobotEmotion emotion = static_cast<RobotEmotion>(currentEmotionInt);

        switch (emotion) {
        case RobotEmotion::Dasara: {
            // Festive Diya lamp flicker
            float f1 = std::sin(animTimer * 12.0f);
            float f2 = std::sin(animTimer * 29.0f);
            float flicker = 0.65f + 0.20f * f1 + 0.15f * f2;
            uint8_t b = (uint8_t)std::clamp((int)std::round((float)userBrightness * flicker), 0, 255);
            applyHardwareDuty(calculateDutyFromBrightness(b));
            break;
        }
        case RobotEmotion::Happy: {
            // Uplifting rhythmic breath
            float phase = (animTimer / 1.4f) * 2.0f * (float)M_PI;
            float factor = 0.20f + 0.80f * (0.5f + 0.5f * std::sin(phase));
            applyHardwareDuty(calculateDutyFromBrightness((uint8_t)(userBrightness * factor)));
            break;
        }
        case RobotEmotion::Curious: {
            // Active searching pulse
            float phase = (animTimer / 0.9f) * 2.0f * (float)M_PI;
            float factor = 0.25f + 0.75f * (0.5f + 0.5f * std::sin(phase));
            applyHardwareDuty(calculateDutyFromBrightness((uint8_t)(userBrightness * factor)));
            break;
        }
        case RobotEmotion::Surprised: {
            // Fast alert strobe (4 Hz)
            bool on = std::fmod(animTimer, 0.25f) < 0.125f;
            applyHardwareDuty(on ? calculateDutyFromBrightness(userBrightness) : 0);
            break;
        }
        case RobotEmotion::Focused: {
            // Solid locked beam (100% user brightness)
            applyHardwareDuty(calculateDutyFromBrightness(userBrightness));
            break;
        }
        case RobotEmotion::Sleepy: {
            // Soft slow breath
            float phase = (animTimer / 4.5f) * 2.0f * (float)M_PI;
            float sine = 0.5f + 0.5f * std::sin(phase);
            float factor = 0.05f + 0.35f * (sine * sine);
            applyHardwareDuty(calculateDutyFromBrightness((uint8_t)(userBrightness * factor)));
            break;
        }
        case RobotEmotion::Wink: {
            // Playful double-wink blip
            float cycle = std::fmod(animTimer, 1.0f);
            bool blip = (cycle < 0.08f) || (cycle >= 0.16f && cycle < 0.24f);
            applyHardwareDuty(blip ? calculateDutyFromBrightness(userBrightness) : calculateDutyFromBrightness((uint8_t)(userBrightness * 0.2f)));
            break;
        }
        case RobotEmotion::Sad: {
            // Dim melancholy glow
            applyHardwareDuty(calculateDutyFromBrightness((uint8_t)(userBrightness * 0.25f)));
            break;
        }
        case RobotEmotion::Rage: {
            // High-speed warning strobe (8 Hz)
            bool strobe = std::fmod(animTimer, 0.125f) < 0.0625f;
            applyHardwareDuty(strobe ? calculateDutyFromBrightness(userBrightness) : 0);
            break;
        }
        case RobotEmotion::Neutral:
        default: {
            // Calm cybernetic pulse
            float phase = (animTimer / 2.8f) * 2.0f * (float)M_PI;
            float factor = 0.15f + 0.85f * (0.5f + 0.5f * std::sin(phase));
            applyHardwareDuty(calculateDutyFromBrightness((uint8_t)(userBrightness * factor)));
            break;
        }
        }
        break;
    }
    }
}

RearLed& getRearLed() {
    static RearLed instance;
    return instance;
}
