#include "RobotFace.hpp"
#include <esp_log.h>
#include <algorithm>
#include <cstdlib>

static const char* TAG = "ROBOT_FACE";

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// Helper: blend two RGB565 colors
uint16_t RobotFace::blendColor(uint16_t c1, uint16_t c2, float t) {
    if (t <= 0.0f) return c1;
    if (t >= 1.0f) return c2;

    uint32_t r1 = (c1 >> 11) & 0x1F;
    uint32_t g1 = (c1 >> 5) & 0x3F;
    uint32_t b1 = c1 & 0x1F;

    uint32_t r2 = (c2 >> 11) & 0x1F;
    uint32_t g2 = (c2 >> 5) & 0x3F;
    uint32_t b2 = c2 & 0x1F;

    uint32_t r = (uint32_t)(r1 + (r2 - r1) * t);
    uint32_t g = (uint32_t)(g1 + (g2 - g1) * t);
    uint32_t b = (uint32_t)(b1 + (b2 - b1) * t);

    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Preset color themes for emotions
RobotTheme RobotFace::getEmotionTheme(RobotEmotion emotion) {
    // Visor obsidian background: 0x0842 (RGB: 8, 8, 16)
    constexpr uint16_t BG = 0x0842;

    switch (emotion) {
        case RobotEmotion::Happy:
            // Cyber Emerald & Coral Blush
            return {
                .primary   = 0x07EC, // Neon Emerald Green
                .secondary = 0xE7FF, // Crisp Mint White
                .glow      = 0x0284, // Deep Emerald Bloom
                .accent    = 0xF9EF, // Coral Pink
                .bg        = BG
            };
        case RobotEmotion::Curious:
            // Electric Sky Azure & Amber
            return {
                .primary   = 0x567F, // Azure Cyan
                .secondary = 0xFFFF, // Pure White
                .glow      = 0x1292, // Deep Azure
                .accent    = 0xFDE0, // Golden Amber
                .bg        = BG
            };
        case RobotEmotion::Surprised:
            // High-voltage Cyber Gold / Yellow
            return {
                .primary   = 0xFFE0, // Vivid Yellow
                .secondary = 0xFFFF, // Pure White
                .glow      = 0x6300, // Amber Glow
                .accent    = 0xFFFF, // Laser White
                .bg        = BG
            };
        case RobotEmotion::Focused:
            // Laser Cyan & Ruby Reticle
            return {
                .primary   = 0x07FD, // Electric Cyan-Teal
                .secondary = 0xFFFF, // Crisp White
                .glow      = 0x024E, // Deep Teal
                .accent    = 0xF800, // Targeting Ruby
                .bg        = BG
            };
        case RobotEmotion::Sleepy:
            // Dreamy Lavender & Soft Cyan
            return {
                .primary   = 0x9BDF, // Soft Lavender
                .secondary = 0xDEFB, // Pale Lilac
                .glow      = 0x28B0, // Deep Violet
                .accent    = 0x5EBF, // Dream Cyan
                .bg        = BG
            };
        case RobotEmotion::Wink:
            // Playful Cyan & Radiant Starburst
            return {
                .primary   = 0x077F, // Neon Cyan
                .secondary = 0xFFFF, // Pure White
                .glow      = 0x0210, // Cyan Glow
                .accent    = 0xFFE0, // Radiant Gold Star
                .bg        = BG
            };
        case RobotEmotion::Sad:
            // Deep Ice Cobalt & Melancholy Navy
            return {
                .primary   = 0x3C1F, // Cobalt Blue
                .secondary = 0x9E7F, // Ice Blue
                .glow      = 0x0989, // Deep Navy
                .accent    = 0x5EBF, // Pale Aqua
                .bg        = BG
            };
        case RobotEmotion::Rage:
            // Fiery Crimson & Overheat Orange
            return {
                .primary   = 0xFA00, // Fiery Red
                .secondary = 0xFFFF, // Blinding White
                .glow      = 0x4800, // Dark Crimson
                .accent    = 0xFDE0, // Overheat Amber
                .bg        = BG
            };
        case RobotEmotion::Neutral:
        default:
            // Signature Cyber Cyan Companion
            return {
                .primary   = 0x077F, // Electric Cyan
                .secondary = 0xFFFF, // Pure White
                .glow      = 0x0210, // Dark Cyan Bloom
                .accent    = 0x3FF7, // Neon Teal
                .bg        = BG
            };
    }
}

RobotFace::RobotFace(LGFX& disp)
    : display(disp)
    , canvas(&disp)
    , canvasReady(false)
    , currentEmotion(RobotEmotion::Neutral)
    , targetEmotion(RobotEmotion::Neutral)
    , themeTransition(1.0f)
    , gazeX(0.0f), targetGazeX(0.0f)
    , gazeY(0.0f), targetGazeY(0.0f)
    , eyeWidth(EYE_BASE_WIDTH), targetEyeWidth(EYE_BASE_WIDTH)
    , eyeHeight(EYE_BASE_HEIGHT), targetEyeHeight(EYE_BASE_HEIGHT)
    , eyeRadius(EYE_BASE_RADIUS), targetEyeRadius(EYE_BASE_RADIUS)
    , upperLidOpen(1.0f), targetUpperLidOpen(1.0f)
    , lowerLidOpen(1.0f), targetLowerLidOpen(1.0f)
    , lidAngle(0.0f), targetLidAngle(0.0f)
    , leftBrowY(52.0f), targetLeftBrowY(52.0f)
    , rightBrowY(52.0f), targetRightBrowY(52.0f)
    , leftBrowAngle(0.0f), targetLeftBrowAngle(0.0f)
    , rightBrowAngle(0.0f), targetRightBrowAngle(0.0f)
    , blinkPhase(BlinkPhase::Idle)
    , blinkTimer(0.0f)
    , blinkProgress(0.0f)
    , nextBlinkInterval(3.5f)
    , isDoubleBlink(false)
    , isWinking(false)
    , saccadeTimer(0.0f)
    , nextSaccadeInterval(2.5f)
    , autoEmotionTimer(0.0f)
    , nextAutoEmotionInterval(12.0f)
    , touchReleaseTimer(0.0f)
    , isUserTouching(false)
    , isBeingPetted(false)
    , globalTime(0.0f)
    , breathPhase(0.0f)
    , bounceOffset(0.0f)
    , voicePhase(0.0f)
    , blushIntensity(0.0f), targetBlushIntensity(0.0f)
    , wifiEnabled(false)
    , wifiIsAp(false)
    , wifiConnected(false)
    , wifiRssi(0)
    , wifiClientCount(0)
    , sdCardMounted(false)
{
    currentTheme = getEmotionTheme(RobotEmotion::Neutral);
    targetTheme = currentTheme;
    for (int i = 0; i < 9; ++i) {
        voiceHeights[i] = 10.0f;
    }
}

RobotFace::~RobotFace() {
    if (canvasReady) {
        canvas.deleteSprite();
    }
}

bool RobotFace::init() {
    ESP_LOGI(TAG, "Allocating double-buffered sprite in PSRAM (320x240 RGB565)...");
    canvas.setPsram(true);
    canvas.setColorDepth(16);
    void* buf = canvas.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!buf) {
        ESP_LOGW(TAG, "PSRAM allocation failed, attempting default SRAM allocation...");
        canvas.setPsram(false);
        buf = canvas.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
    }

    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate 320x240 sprite buffer!");
        canvasReady = false;
        return false;
    }

    canvasReady = true;
    ESP_LOGI(TAG, "Sprite initialized successfully (%u bytes).", (unsigned)canvas.bufferLength());

    // Apply neutral preset immediately
    setEmotion(RobotEmotion::Neutral, true);

    return true;
}

void RobotFace::applyEmotionPreset(RobotEmotion emotion) {
    targetTheme = getEmotionTheme(emotion);
    themeTransition = 0.0f;

    switch (emotion) {
        case RobotEmotion::Happy:
            targetEyeWidth         = EYE_BASE_WIDTH * 0.95f;
            targetEyeHeight        = EYE_BASE_HEIGHT * 0.85f;
            targetEyeRadius        = EYE_BASE_RADIUS;
            targetUpperLidOpen     = 1.0f;
            targetLowerLidOpen     = 1.0f;
            targetLidAngle         = 0.0f;
            targetLeftBrowY        = 42.0f;
            targetRightBrowY       = 42.0f;
            targetLeftBrowAngle    = -0.10f;
            targetRightBrowAngle   = 0.10f;
            targetBlushIntensity   = 1.0f;
            isWinking              = false;
            break;

        case RobotEmotion::Curious:
            targetEyeWidth         = EYE_BASE_WIDTH;
            targetEyeHeight        = EYE_BASE_HEIGHT * 1.05f;
            targetEyeRadius        = EYE_BASE_RADIUS;
            targetUpperLidOpen     = 0.95f;
            targetLowerLidOpen     = 0.95f;
            targetLidAngle         = 0.12f;
            // Cocked right eyebrow, slightly furrowed left
            targetLeftBrowY        = 56.0f;
            targetRightBrowY       = 36.0f;
            targetLeftBrowAngle    = -0.15f;
            targetRightBrowAngle   = 0.28f;
            targetBlushIntensity   = 0.0f;
            targetGazeX            = 14.0f;
            targetGazeY            = -10.0f;
            isWinking              = false;
            break;

        case RobotEmotion::Surprised:
            targetEyeWidth         = EYE_BASE_WIDTH * 1.15f;
            targetEyeHeight        = EYE_BASE_HEIGHT * 1.20f;
            targetEyeRadius        = EYE_BASE_RADIUS * 1.25f;
            targetUpperLidOpen     = 1.0f;
            targetLowerLidOpen     = 1.0f;
            targetLidAngle         = 0.0f;
            targetLeftBrowY        = 34.0f;
            targetRightBrowY       = 34.0f;
            targetLeftBrowAngle    = 0.0f;
            targetRightBrowAngle   = 0.0f;
            targetBlushIntensity   = 0.0f;
            isWinking              = false;
            break;

        case RobotEmotion::Focused:
            targetEyeWidth         = EYE_BASE_WIDTH * 1.05f;
            targetEyeHeight        = EYE_BASE_HEIGHT * 0.82f;
            targetEyeRadius        = EYE_BASE_RADIUS * 0.85f;
            targetUpperLidOpen     = 0.78f;
            targetLowerLidOpen     = 0.90f;
            targetLidAngle         = 0.26f; // Fierce inward slant
            targetLeftBrowY        = 58.0f;
            targetRightBrowY       = 58.0f;
            targetLeftBrowAngle    = 0.26f;
            targetRightBrowAngle   = -0.26f;
            targetBlushIntensity   = 0.0f;
            isWinking              = false;
            break;

        case RobotEmotion::Sleepy:
            targetEyeWidth         = EYE_BASE_WIDTH * 0.95f;
            targetEyeHeight        = EYE_BASE_HEIGHT * 0.70f;
            targetEyeRadius        = EYE_BASE_RADIUS;
            targetUpperLidOpen     = 0.32f; // Heavy drooping lid
            targetLowerLidOpen     = 0.85f;
            targetLidAngle         = -0.05f;
            targetLeftBrowY        = 58.0f;
            targetRightBrowY       = 58.0f;
            targetLeftBrowAngle    = 0.02f;
            targetRightBrowAngle   = -0.02f;
            targetBlushIntensity   = 0.0f;
            targetGazeX            = 0.0f;
            targetGazeY            = 12.0f; // Drooping gaze
            isWinking              = false;
            break;

        case RobotEmotion::Wink:
            targetEyeWidth         = EYE_BASE_WIDTH;
            targetEyeHeight        = EYE_BASE_HEIGHT;
            targetEyeRadius        = EYE_BASE_RADIUS;
            targetUpperLidOpen     = 1.0f;
            targetLowerLidOpen     = 1.0f;
            targetLidAngle         = 0.0f;
            targetLeftBrowY        = 46.0f;
            targetRightBrowY       = 52.0f;
            targetLeftBrowAngle    = -0.08f;
            targetRightBrowAngle   = 0.05f;
            targetBlushIntensity   = 0.85f;
            isWinking              = true;
            break;

        case RobotEmotion::Sad:
            targetEyeWidth         = EYE_BASE_WIDTH * 0.95f;
            targetEyeHeight        = EYE_BASE_HEIGHT * 0.90f;
            targetEyeRadius        = EYE_BASE_RADIUS;
            targetUpperLidOpen     = 0.75f;
            targetLowerLidOpen     = 0.85f;
            targetLidAngle         = -0.20f; // Outward droop
            targetLeftBrowY        = 46.0f;
            targetRightBrowY       = 46.0f;
            targetLeftBrowAngle    = -0.25f; // Raised inner brows
            targetRightBrowAngle   = 0.25f;
            targetBlushIntensity   = 0.0f;
            targetGazeX            = 0.0f;
            targetGazeY            = 8.0f;
            isWinking              = false;
            break;

        case RobotEmotion::Rage:
            targetEyeWidth         = EYE_BASE_WIDTH * 1.10f;
            targetEyeHeight        = EYE_BASE_HEIGHT * 0.78f;
            targetEyeRadius        = EYE_BASE_RADIUS * 0.8f;
            targetUpperLidOpen     = 0.70f;
            targetLowerLidOpen     = 0.95f;
            targetLidAngle         = 0.35f; // Sharp angry inward slant
            targetLeftBrowY        = 60.0f;
            targetRightBrowY       = 60.0f;
            targetLeftBrowAngle    = 0.35f;
            targetRightBrowAngle   = -0.35f;
            targetBlushIntensity   = 0.0f;
            isWinking              = false;
            break;

        case RobotEmotion::Neutral:
        default:
            targetEyeWidth         = EYE_BASE_WIDTH;
            targetEyeHeight        = EYE_BASE_HEIGHT;
            targetEyeRadius        = EYE_BASE_RADIUS;
            targetUpperLidOpen     = 1.0f;
            targetLowerLidOpen     = 1.0f;
            targetLidAngle         = 0.0f;
            targetLeftBrowY        = 50.0f;
            targetRightBrowY       = 50.0f;
            targetLeftBrowAngle    = 0.0f;
            targetRightBrowAngle   = 0.0f;
            targetBlushIntensity   = 0.0f;
            isWinking              = false;
            break;
    }
}

void RobotFace::setEmotion(RobotEmotion emotion, bool immediate) {
    targetEmotion = emotion;
    applyEmotionPreset(emotion);

    if (immediate) {
        currentEmotion       = emotion;
        currentTheme         = targetTheme;
        themeTransition      = 1.0f;
        eyeWidth             = targetEyeWidth;
        eyeHeight            = targetEyeHeight;
        eyeRadius            = targetEyeRadius;
        upperLidOpen         = targetUpperLidOpen;
        lowerLidOpen         = targetLowerLidOpen;
        lidAngle             = targetLidAngle;
        leftBrowY            = targetLeftBrowY;
        rightBrowY           = targetRightBrowY;
        leftBrowAngle        = targetLeftBrowAngle;
        rightBrowAngle       = targetRightBrowAngle;
        blushIntensity       = targetBlushIntensity;
    }
}

void RobotFace::nextEmotion() {
    int next = ((int)currentEmotion + 1) % (int)RobotEmotion::COUNT;
    setEmotion((RobotEmotion)next, false);
}

void RobotFace::setLookAt(float normX, float normY) {
    targetGazeX = std::max(-26.0f, std::min(26.0f, normX * 26.0f));
    targetGazeY = std::max(-20.0f, std::min(20.0f, normY * 20.0f));
}

void RobotFace::setWifiStatus(bool enabled, bool isAp, bool isConnected, int8_t rssi, uint8_t clientCount) {
    wifiEnabled = enabled;
    wifiIsAp = isAp;
    wifiConnected = isConnected;
    wifiRssi = rssi;
    wifiClientCount = clientCount;
}

void RobotFace::setSdCardStatus(bool mounted) {
    sdCardMounted = mounted;
}

void RobotFace::handleTouch(int32_t x, int32_t y) {
    isUserTouching = true;
    touchReleaseTimer = 1.6f; // Stay engaged for 1.6s after touch

    // Map screen coordinate (0..319, 0..239) to normalized gaze (-1.0 to 1.0)
    float nx = (float)(x - SCREEN_WIDTH / 2) / (float)(SCREEN_WIDTH / 2);
    float ny = (float)(y - SCREEN_HEIGHT / 2) / (float)(SCREEN_HEIGHT / 2);
    setLookAt(nx, ny);

    // Forehead petting region: top center area (y < 70, 90 <= x <= 230)
    if (y < 70 && x >= 90 && x <= 230) {
        if (!isBeingPetted) {
            isBeingPetted = true;
            setEmotion(RobotEmotion::Happy, false);
        }
    }
}

void RobotFace::releaseTouch() {
    isUserTouching = false;
    isBeingPetted = false;
}

void RobotFace::triggerBlink() {
    if (blinkPhase == BlinkPhase::Idle) {
        blinkPhase = BlinkPhase::Closing;
        blinkProgress = 0.0f;
        isDoubleBlink = (rand() % 4 == 0); // 25% double-blink chance
    }
}

void RobotFace::triggerWink() {
    setEmotion(RobotEmotion::Wink, false);
}

void RobotFace::updateBlinking(float dt) {
    if (isWinking) {
        return; // Wink holds eyelid state separately
    }

    switch (blinkPhase) {
        case BlinkPhase::Idle:
            blinkTimer += dt;
            if (blinkTimer >= nextBlinkInterval) {
                blinkTimer = 0.0f;
                nextBlinkInterval = 2.8f + (float)(rand() % 35) * 0.1f; // 2.8s to 6.3s
                triggerBlink();
            }
            break;

        case BlinkPhase::Closing:
            blinkProgress += dt / 0.045f; // Rapid close in 45ms
            if (blinkProgress >= 1.0f) {
                blinkProgress = 1.0f;
                blinkPhase = BlinkPhase::Closed;
                blinkTimer = 0.0f;
            }
            break;

        case BlinkPhase::Closed:
            blinkTimer += dt;
            if (blinkTimer >= 0.025f) { // Stay closed for 25ms
                blinkPhase = BlinkPhase::Opening;
                blinkProgress = 0.0f;
            }
            break;

        case BlinkPhase::Opening:
            blinkProgress += dt / 0.085f; // Re-open in 85ms
            if (blinkProgress >= 1.0f) {
                blinkProgress = 1.0f;
                if (isDoubleBlink) {
                    isDoubleBlink = false;
                    blinkPhase = BlinkPhase::Closing;
                    blinkProgress = 0.2f; // Quick flutter close
                } else {
                    blinkPhase = BlinkPhase::Idle;
                    blinkTimer = 0.0f;
                }
            }
            break;
    }
}

void RobotFace::updateAutonomousAI(float dt) {
    if (touchReleaseTimer > 0.0f) {
        touchReleaseTimer -= dt;
        if (touchReleaseTimer <= 0.0f) {
            releaseTouch();
            // Return to calm neutral state if it was petted
            if (currentEmotion == RobotEmotion::Happy) {
                setEmotion(RobotEmotion::Neutral, false);
            }
        }
        return; // User has control, skip autonomous drift
    }

    // Autonomous Saccades (Lifelike gaze shifts)
    saccadeTimer += dt;
    if (saccadeTimer >= nextSaccadeInterval) {
        saccadeTimer = 0.0f;
        nextSaccadeInterval = 1.8f + (float)(rand() % 30) * 0.1f;

        int r = rand() % 100;
        if (r < 45) {
            // Look directly at user
            targetGazeX = 0.0f;
            targetGazeY = 0.0f;
        } else if (r < 65) {
            // Glance left or right
            targetGazeX = (rand() % 2 == 0) ? -18.0f : 18.0f;
            targetGazeY = -2.0f;
        } else if (r < 85) {
            // Thoughtful upward glance
            targetGazeX = (rand() % 2 == 0) ? -14.0f : 14.0f;
            targetGazeY = -12.0f;
        } else {
            // Downward glance
            targetGazeX = 0.0f;
            targetGazeY = 10.0f;
        }
    }

    // Autonomous Emotion Drift (Cycle through behaviors over time)
    autoEmotionTimer += dt;
    if (autoEmotionTimer >= nextAutoEmotionInterval) {
        autoEmotionTimer = 0.0f;
        nextAutoEmotionInterval = 9.0f + (float)(rand() % 60) * 0.1f; // 9s to 15s

        // Sequence of engaging personality states
        static const RobotEmotion sequence[] = {
            RobotEmotion::Neutral,
            RobotEmotion::Curious,
            RobotEmotion::Neutral,
            RobotEmotion::Happy,
            RobotEmotion::Focused,
            RobotEmotion::Neutral,
            RobotEmotion::Wink,
            RobotEmotion::Neutral,
            RobotEmotion::Sleepy,
            RobotEmotion::Surprised
        };
        static size_t seqIndex = 0;
        seqIndex = (seqIndex + 1) % (sizeof(sequence) / sizeof(sequence[0]));
        setEmotion(sequence[seqIndex], false);
    }
}

void RobotFace::updateTheme(float dt) {
    if (themeTransition < 1.0f) {
        themeTransition += dt * 3.5f; // Smooth 0.28s color fade
        if (themeTransition > 1.0f) themeTransition = 1.0f;

        currentTheme.primary   = blendColor(currentTheme.primary, targetTheme.primary, themeTransition);
        currentTheme.secondary = blendColor(currentTheme.secondary, targetTheme.secondary, themeTransition);
        currentTheme.glow      = blendColor(currentTheme.glow, targetTheme.glow, themeTransition);
        currentTheme.accent    = blendColor(currentTheme.accent, targetTheme.accent, themeTransition);
        currentTheme.bg        = targetTheme.bg;

        if (themeTransition >= 1.0f) {
            currentTheme = targetTheme;
            currentEmotion = targetEmotion;
        }
    }
}

void RobotFace::updateKinematics(float dt) {
    // Spring-damped exponential easing (frame-rate independent)
    const float posSpeed  = 14.0f;
    const float lidSpeed  = 16.0f;
    const float browSpeed = 12.0f;
    const float sizeSpeed = 10.0f;

    float posAlpha  = 1.0f - expf(-posSpeed * dt);
    float lidAlpha  = 1.0f - expf(-lidSpeed * dt);
    float browAlpha = 1.0f - expf(-browSpeed * dt);
    float sizeAlpha = 1.0f - expf(-sizeSpeed * dt);

    gazeX += (targetGazeX - gazeX) * posAlpha;
    gazeY += (targetGazeY - gazeY) * posAlpha;

    eyeWidth  += (targetEyeWidth - eyeWidth) * sizeAlpha;
    eyeHeight += (targetEyeHeight - eyeHeight) * sizeAlpha;
    eyeRadius += (targetEyeRadius - eyeRadius) * sizeAlpha;

    upperLidOpen += (targetUpperLidOpen - upperLidOpen) * lidAlpha;
    lowerLidOpen += (targetLowerLidOpen - lowerLidOpen) * lidAlpha;
    lidAngle     += (targetLidAngle - lidAngle) * lidAlpha;

    leftBrowY      += (targetLeftBrowY - leftBrowY) * browAlpha;
    rightBrowY     += (targetRightBrowY - rightBrowY) * browAlpha;
    leftBrowAngle  += (targetLeftBrowAngle - leftBrowAngle) * browAlpha;
    rightBrowAngle += (targetRightBrowAngle - rightBrowAngle) * browAlpha;

    blushIntensity += (targetBlushIntensity - blushIntensity) * (1.0f - expf(-8.0f * dt));
}

void RobotFace::update(float dt) {
    globalTime += dt;

    // Organic breathing cycle (~3.5s per breath)
    breathPhase += dt * 1.8f;
    if (breathPhase > 2.0f * M_PI) breathPhase -= 2.0f * M_PI;

    // Cheerful bounce when happy or petted
    if (currentEmotion == RobotEmotion::Happy || isBeingPetted) {
        bounceOffset = fabsf(sinf(globalTime * 9.0f)) * 4.5f;
    } else {
        bounceOffset = 0.0f;
    }

    // Dynamic voice waveform synthesis
    voicePhase += dt * 8.0f;
    for (int i = 0; i < 9; ++i) {
        float harmonic = sinf(voicePhase + (float)i * 0.8f) * cosf(voicePhase * 0.6f + (float)i * 0.4f);
        float baseH = 5.0f + 16.0f * (1.0f - fabsf((float)i - 4.0f) / 5.2f);
        if (currentEmotion == RobotEmotion::Sleepy) baseH *= 0.35f;
        if (currentEmotion == RobotEmotion::Rage) baseH *= 1.35f;
        voiceHeights[i] = baseH + fabsf(harmonic) * (baseH * 0.7f);
    }

    // Run behaviors
    updateAutonomousAI(dt);
    updateBlinking(dt);
    updateTheme(dt);
    updateKinematics(dt);
}

// Render the entire cybernetic robot face
void RobotFace::render() {
    if (!canvasReady) return;

    // 1. Draw visor background with tech brackets
    drawBackground();

    // 2. Draw cybernetic visor HUD
    drawVisorHUD();

    // Compute dynamic offsets
    float breathY = sinf(breathPhase) * 1.6f;
    float effectiveLeftEyeY  = EYE_NOMINAL_Y + gazeY + breathY - bounceOffset;
    float effectiveRightEyeY = EYE_NOMINAL_Y + gazeY + breathY - bounceOffset;
    float effectiveLeftEyeX  = EYE_CENTER_LEFT_X + gazeX;
    float effectiveRightEyeX = EYE_CENTER_RIGHT_X + gazeX;

    // Compute blink factor
    float currentUpperLid = upperLidOpen;
    if (blinkPhase == BlinkPhase::Closing) {
        currentUpperLid = std::min(upperLidOpen, 1.0f - blinkProgress);
    } else if (blinkPhase == BlinkPhase::Closed) {
        currentUpperLid = 0.0f;
    } else if (blinkPhase == BlinkPhase::Opening) {
        currentUpperLid = std::min(upperLidOpen, blinkProgress);
    }

    // 3. Draw Eyes
    if (currentEmotion == RobotEmotion::Happy) {
        drawHappyEye(effectiveLeftEyeX, effectiveLeftEyeY, true);
        drawHappyEye(effectiveRightEyeX, effectiveRightEyeY, false);
    } else if (isWinking) {
        drawWinkEye(effectiveLeftEyeX, effectiveLeftEyeY, true);
        drawEye(effectiveRightEyeX, effectiveRightEyeY, false, currentUpperLid, lowerLidOpen, lidAngle);
    } else {
        drawEye(effectiveLeftEyeX, effectiveLeftEyeY, true, currentUpperLid, lowerLidOpen, lidAngle);
        drawEye(effectiveRightEyeX, effectiveRightEyeY, false, currentUpperLid, lowerLidOpen, lidAngle);
    }

    // 4. Draw Eyebrows
    float browBob = sinf(breathPhase) * 0.8f - bounceOffset;
    drawEyebrow(EYE_CENTER_LEFT_X + gazeX * 0.5f, leftBrowY + browBob, leftBrowAngle, 46.0f);
    drawEyebrow(EYE_CENTER_RIGHT_X + gazeX * 0.5f, rightBrowY + browBob, rightBrowAngle, 46.0f);

    // 5. Draw Cheek Blush
    drawCheekBlush();

    // 6. Draw Mouth / Voice Synthesizer
    drawMouth();

    // 7. Draw Holographic Accents (Curious spinner, Alert !, Sleepy Z)
    drawHolographicAccents();

    // Push completed frame to physical screen via high-speed SPI
    canvas.pushSprite(0, 0);
}

void RobotFace::drawBackground() {
    canvas.fillScreen(currentTheme.bg);

    // Subtle Sci-Fi Corner Brackets
    constexpr int32_t B_LEN = 14;
    uint16_t bracketCol = currentTheme.glow;

    // Top-Left ┌
    canvas.drawFastHLine(10, 10, B_LEN, bracketCol);
    canvas.drawFastVLine(10, 10, B_LEN, bracketCol);
    canvas.drawPixel(12, 12, currentTheme.primary);

    // Top-Right ┐
    canvas.drawFastHLine(SCREEN_WIDTH - 10 - B_LEN, 10, B_LEN, bracketCol);
    canvas.drawFastVLine(SCREEN_WIDTH - 11, 10, B_LEN, bracketCol);
    canvas.drawPixel(SCREEN_WIDTH - 13, 12, currentTheme.primary);

    // Bottom-Left └
    canvas.drawFastHLine(10, SCREEN_HEIGHT - 11, B_LEN, bracketCol);
    canvas.drawFastVLine(10, SCREEN_HEIGHT - 10 - B_LEN, B_LEN, bracketCol);
    canvas.drawPixel(12, SCREEN_HEIGHT - 13, currentTheme.primary);

    // Bottom-Right ┘
    canvas.drawFastHLine(SCREEN_WIDTH - 10 - B_LEN, SCREEN_HEIGHT - 11, B_LEN, bracketCol);
    canvas.drawFastVLine(SCREEN_WIDTH - 11, SCREEN_HEIGHT - 10 - B_LEN, B_LEN, bracketCol);
    canvas.drawPixel(SCREEN_WIDTH - 13, SCREEN_HEIGHT - 13, currentTheme.primary);
}

void RobotFace::drawVisorHUD() {
    // 1. Top-Left Battery Indicator
    constexpr int32_t bx = 16, by = 13;
    canvas.drawRoundRect(bx, by, 18, 9, 2, currentTheme.glow);
    canvas.fillRect(bx + 18, by + 2, 2, 5, currentTheme.glow);
    // 3 battery cell bars
    canvas.fillRect(bx + 2, by + 2, 3, 5, currentTheme.accent);
    canvas.fillRect(bx + 7, by + 2, 3, 5, currentTheme.accent);
    canvas.fillRect(bx + 12, by + 2, 3, 5, currentTheme.accent);

    // 2. Status Label
    canvas.setTextSize(1);
    canvas.setTextColor(currentTheme.primary, currentTheme.bg);
    const char* statusStr = "SYS.OK";
    switch (currentEmotion) {
        case RobotEmotion::Happy:     statusStr = "AI.HAPPY";   break;
        case RobotEmotion::Curious:   statusStr = "AI.SCAN";    break;
        case RobotEmotion::Surprised: statusStr = "ALERT!!";    break;
        case RobotEmotion::Focused:   statusStr = "LOCK.ON";    break;
        case RobotEmotion::Sleepy:    statusStr = "SYS.REST";   break;
        case RobotEmotion::Wink:      statusStr = "LINK.SYNC";  break;
        case RobotEmotion::Sad:       statusStr = "DIAG.FAIL";  break;
        case RobotEmotion::Rage:      statusStr = "WARN.HOT";   break;
        default:                      statusStr = "SYS.ONLINE"; break;
    }
    canvas.drawString(statusStr, bx + 26, by + 1);

    // 2b. Cybernetic Wi-Fi / Hotspot HUD Indicator
    if (wifiEnabled) {
        constexpr int32_t wx = 122, wy = by;
        if (wifiIsAp) {
            // Hotspot mode: "AP" + broadcast radiation rings
            canvas.setTextSize(1);
            canvas.setTextColor(currentTheme.accent, currentTheme.bg);
            canvas.drawString("AP", wx, wy + 1);

            int32_t cx = wx + 18, cy = wy + 4;
            canvas.drawCircle(cx, cy, 2, currentTheme.primary);
            canvas.drawCircle(cx, cy, 5, currentTheme.glow);

            if (wifiClientCount > 0) {
                char clientStr[8];
                snprintf(clientStr, sizeof(clientStr), ":%u", (unsigned int)wifiClientCount);
                canvas.setTextColor(currentTheme.secondary, currentTheme.bg);
                canvas.drawString(clientStr, cx + 8, wy + 1);
            }
        } else if (wifiConnected) {
            // Station mode: 3-bar signal strength graph
            constexpr int32_t sx = wx;
            uint16_t col1 = (wifiRssi >= -85) ? currentTheme.accent : currentTheme.glow;
            uint16_t col2 = (wifiRssi >= -70) ? currentTheme.accent : currentTheme.glow;
            uint16_t col3 = (wifiRssi >= -55) ? currentTheme.accent : currentTheme.glow;

            canvas.fillRect(sx,     wy + 6, 2, 3, col1);
            canvas.fillRect(sx + 4, wy + 4, 2, 5, col2);
            canvas.fillRect(sx + 8, wy + 2, 2, 7, col3);

            canvas.setTextSize(1);
            canvas.setTextColor(currentTheme.primary, currentTheme.bg);
            canvas.drawString("NET", sx + 14, wy + 1);
        } else {
            // Connecting sync pulse
            canvas.setTextSize(1);
            bool blink = fmodf(globalTime * 2.0f, 1.0f) < 0.5f;
            canvas.setTextColor(blink ? currentTheme.glow : currentTheme.primary, currentTheme.bg);
            canvas.drawString("LINK...", wx, wy + 1);
        }
    }

    // 2c. Cybernetic SD Card Status Indicator
    if (sdCardMounted) {
        constexpr int32_t sdx = 188, sdy = by;
        canvas.setTextSize(1);
        canvas.setTextColor(currentTheme.accent, currentTheme.bg);
        canvas.drawString("SD", sdx, sdy + 1);
        // Micro-SD card outline badge
        canvas.drawRect(sdx + 14, sdy + 1, 6, 8, currentTheme.primary);
        canvas.drawPixel(sdx + 14, sdy + 1, currentTheme.bg);
    }

    // 3. Top-Right Neural Telemetry ECG Wave
    constexpr int32_t hx = SCREEN_WIDTH - 64, hy = by + 4;
    canvas.drawFastHLine(hx, hy, 8, currentTheme.glow);
    // Animated heartbeat spike
    float pulse = fmodf(globalTime * 3.0f, 1.0f);
    int32_t spike = (pulse < 0.25f) ? (int32_t)(sinf(pulse * 4.0f * M_PI) * 5.0f) : 0;
    canvas.drawLine(hx + 8, hy, hx + 12, hy - spike, currentTheme.primary);
    canvas.drawLine(hx + 12, hy - spike, hx + 16, hy + spike / 2, currentTheme.primary);
    canvas.drawLine(hx + 16, hy + spike / 2, hx + 20, hy, currentTheme.primary);
    canvas.drawFastHLine(hx + 20, hy, 16, currentTheme.glow);

    // Touch feedback beacon
    if (isUserTouching) {
        canvas.fillCircle(SCREEN_WIDTH - 18, by + 4, 3, currentTheme.accent);
    } else {
        canvas.drawCircle(SCREEN_WIDTH - 18, by + 4, 3, currentTheme.glow);
    }
}

void RobotFace::drawEye(float cx, float cy, bool isLeft, float upperOpen, float lowerOpen, float slant) {
    int32_t w = (int32_t)eyeWidth;
    int32_t h = (int32_t)eyeHeight;
    int32_t r = (int32_t)eyeRadius;

    // Completely shut sleep line
    if (upperOpen <= 0.04f && lowerOpen <= 0.04f) {
        canvas.drawWideLine(cx - w / 2, cy, cx + w / 2, cy, 3.5f, currentTheme.primary);
        canvas.drawWideLine(cx - w / 2, cy, cx + w / 2, cy, 1.5f, currentTheme.secondary);
        return;
    }

    // 1. Outer Glow / Halo
    canvas.fillSmoothRoundRect((int32_t)(cx - (w + 8) / 2), (int32_t)(cy - (h + 8) / 2),
                               w + 8, h + 8, r + 3, currentTheme.glow);

    // 2. Main Vibrant Iris Body
    canvas.fillSmoothRoundRect((int32_t)(cx - w / 2), (int32_t)(cy - h / 2),
                               w, h, r, currentTheme.primary);

    // 3. Inner Iris Core Accent (Lighter Rim)
    canvas.drawRoundRect((int32_t)(cx - (w - 10) / 2), (int32_t)(cy - (h - 10) / 2),
                         w - 10, h - 10, std::max<int32_t>(2, r - 4), currentTheme.secondary);

    // 4. Dark Cyber Pupil / Lens Core (moves with parallax)
    float pupilShiftX = gazeX * 0.40f;
    float pupilShiftY = gazeY * 0.40f;
    int32_t pr = std::max<int32_t>(10, (int32_t)(r * 0.65f));
    canvas.fillSmoothCircle((int32_t)(cx + pupilShiftX), (int32_t)(cy + pupilShiftY), pr, currentTheme.bg);

    // Inner reticle crosshair inside pupil
    canvas.drawFastHLine((int32_t)(cx + pupilShiftX - 4), (int32_t)(cy + pupilShiftY), 9, currentTheme.glow);
    canvas.drawFastVLine((int32_t)(cx + pupilShiftX), (int32_t)(cy + pupilShiftY - 4), 9, currentTheme.glow);

    // 5. Specular Highlights / Glints (3D glass lens curvature illusion)
    // Primary glint (Upper)
    float glint1X = cx - w * 0.20f - gazeX * 0.15f;
    float glint1Y = cy - h * 0.22f - gazeY * 0.15f;
    canvas.fillSmoothCircle((int32_t)glint1X, (int32_t)glint1Y, 5, TFT_WHITE);
    canvas.fillSmoothCircle((int32_t)(glint1X - 0.5f), (int32_t)(glint1Y - 0.5f), 3, currentTheme.secondary);

    // Secondary micro-glint (Lower)
    float glint2X = cx + w * 0.20f - gazeX * 0.10f;
    float glint2Y = cy + h * 0.20f - gazeY * 0.10f;
    canvas.fillSmoothCircle((int32_t)glint2X, (int32_t)glint2Y, 2, currentTheme.secondary);

    // 6. Eyelid Cutting & Slant
    float dx = (w + 14.0f) / 2.0f;
    float dyTilt = tanf(slant) * dx;
    if (!isLeft) dyTilt = -dyTilt; // Mirror tilt for right eye

    // Upper eyelid shutter
    if (upperOpen < 0.98f) {
        float cutY = (cy - h / 2.0f) + h * (1.0f - upperOpen);
        float yL = cutY - dyTilt;
        float yR = cutY + dyTilt;

        // Fill background over the top
        canvas.fillTriangle((int32_t)(cx - dx - 4), (int32_t)yL,
                            (int32_t)(cx + dx + 4), (int32_t)yR,
                            (int32_t)(cx - dx - 4), (int32_t)(cy - h / 2.0f - 20),
                            currentTheme.bg);
        canvas.fillTriangle((int32_t)(cx + dx + 4), (int32_t)yR,
                            (int32_t)(cx - dx - 4), (int32_t)(cy - h / 2.0f - 20),
                            (int32_t)(cx + dx + 4), (int32_t)(cy - h / 2.0f - 20),
                            currentTheme.bg);

        // Glowing mechanical eyelid rim line
        canvas.drawWideLine(cx - dx, yL, cx + dx, yR, 2.5f, currentTheme.glow);
        canvas.drawWideLine(cx - dx, yL - 1, cx + dx, yR - 1, 1.2f, currentTheme.primary);
    }

    // Lower eyelid squint
    if (lowerOpen < 0.98f) {
        float cutY = (cy + h / 2.0f) - h * (1.0f - lowerOpen);
        float yL = cutY + dyTilt;
        float yR = cutY - dyTilt;

        // Fill background under the bottom
        canvas.fillTriangle((int32_t)(cx - dx - 4), (int32_t)yL,
                            (int32_t)(cx + dx + 4), (int32_t)yR,
                            (int32_t)(cx - dx - 4), (int32_t)(cy + h / 2.0f + 20),
                            currentTheme.bg);
        canvas.fillTriangle((int32_t)(cx + dx + 4), (int32_t)yR,
                            (int32_t)(cx - dx - 4), (int32_t)(cy + h / 2.0f + 20),
                            (int32_t)(cx + dx + 4), (int32_t)(cy + h / 2.0f + 20),
                            currentTheme.bg);

        // Glowing lower rim line
        canvas.drawWideLine(cx - dx, yL, cx + dx, yR, 2.2f, currentTheme.glow);
        canvas.drawWideLine(cx - dx, yL + 1, cx + dx, yR + 1, 1.2f, currentTheme.primary);
    }
}

void RobotFace::drawHappyEye(float cx, float cy, bool isLeft) {
    // Joyful curved crescent anime/cyber arch: ^
    constexpr int NUM_PTS = 11;
    float halfW = eyeWidth * 0.44f;
    float archH = eyeHeight * 0.48f;
    float baseY = cy + archH * 0.35f;

    float ptsX[NUM_PTS];
    float ptsY[NUM_PTS];

    for (int i = 0; i < NUM_PTS; ++i) {
        float t = (float)i / (float)(NUM_PTS - 1); // 0.0 to 1.0
        ptsX[i] = cx - halfW + (2.0f * halfW) * t;
        ptsY[i] = baseY - archH * sinf(t * M_PI);
    }

    // Draw outer glow segments
    for (int i = 0; i < NUM_PTS - 1; ++i) {
        canvas.drawWideLine(ptsX[i], ptsY[i], ptsX[i + 1], ptsY[i + 1], 10.0f, currentTheme.glow);
    }

    // Draw main vibrant arch body
    for (int i = 0; i < NUM_PTS - 1; ++i) {
        canvas.drawWideLine(ptsX[i], ptsY[i], ptsX[i + 1], ptsY[i + 1], 6.5f, currentTheme.primary);
    }

    // Draw laser-white core
    for (int i = 0; i < NUM_PTS - 1; ++i) {
        canvas.drawWideLine(ptsX[i], ptsY[i], ptsX[i + 1], ptsY[i + 1], 2.8f, currentTheme.secondary);
    }

    // Cheerful upturned rounded tips
    canvas.fillSmoothCircle(ptsX[0], ptsY[0], 3.5f, currentTheme.primary);
    canvas.fillSmoothCircle(ptsX[NUM_PTS - 1], ptsY[NUM_PTS - 1], 3.5f, currentTheme.primary);
}

void RobotFace::drawWinkEye(float cx, float cy, bool isLeft) {
    // Left eye winks: sleek closed smiling slit with flutter eyelash
    float halfW = eyeWidth * 0.40f;
    float startX = cx - halfW;
    float endX   = cx + halfW;
    float midY   = cy + 4.0f;

    // Curved smiling line
    canvas.drawWideLine(startX, midY, cx, midY - 6.0f, 4.0f, currentTheme.primary);
    canvas.drawWideLine(cx, midY - 6.0f, endX, midY, 4.0f, currentTheme.primary);
    canvas.drawWideLine(startX, midY, cx, midY - 6.0f, 1.8f, currentTheme.secondary);
    canvas.drawWideLine(cx, midY - 6.0f, endX, midY, 1.8f, currentTheme.secondary);

    // Cute flutter lash at outer edge
    canvas.drawWideLine(startX, midY, startX - 7.0f, midY - 7.0f, 2.5f, currentTheme.primary);
    canvas.drawWideLine(startX, midY, startX - 8.0f, midY - 1.0f, 2.0f, currentTheme.glow);

    // Radiant Starburst Sparkle above the right eye
    float starX = EYE_CENTER_RIGHT_X + gazeX * 0.5f - 18.0f;
    float starY = EYE_NOMINAL_Y + gazeY * 0.5f - 24.0f;
    float starLen = 8.0f + 2.0f * sinf(globalTime * 6.0f);

    canvas.drawWideLine(starX - starLen, starY, starX + starLen, starY, 2.0f, currentTheme.accent);
    canvas.drawWideLine(starX, starY - starLen, starX, starY + starLen, 2.0f, currentTheme.accent);
    canvas.fillSmoothCircle(starX, starY, 3, TFT_WHITE);
}

void RobotFace::drawEyebrow(float cx, float cy, float angle, float length) {
    float halfL = length / 2.0f;
    float cosA = cosf(angle);
    float sinA = sinf(angle);

    float x0 = cx - halfL * cosA;
    float y0 = cy - halfL * sinA;
    float x1 = cx + halfL * cosA;
    float y1 = cy + halfL * sinA;

    // Outer glow
    canvas.drawWideLine(x0, y0, x1, y1, 7.5f, currentTheme.glow);
    // Main brow body
    canvas.drawWideLine(x0, y0, x1, y1, 5.0f, currentTheme.primary);
    // Bright core
    canvas.drawWideLine(x0, y0, x1, y1, 2.0f, currentTheme.secondary);
}

void RobotFace::drawMouth() {
    constexpr int32_t MCX = SCREEN_WIDTH / 2;
    constexpr int32_t MCY = 192;

    if (currentEmotion == RobotEmotion::Happy || isBeingPetted) {
        // Glowing curved smile
        constexpr int NUM_SMILE_PTS = 9;
        float halfW = 28.0f;
        float smileH = 14.0f;
        float ptsX[NUM_SMILE_PTS];
        float ptsY[NUM_SMILE_PTS];

        for (int i = 0; i < NUM_SMILE_PTS; ++i) {
            float t = (float)i / (float)(NUM_SMILE_PTS - 1);
            ptsX[i] = (float)MCX - halfW + (2.0f * halfW) * t;
            ptsY[i] = (float)MCY - 4.0f + smileH * sinf(t * M_PI);
        }

        for (int i = 0; i < NUM_SMILE_PTS - 1; ++i) {
            canvas.drawWideLine(ptsX[i], ptsY[i], ptsX[i + 1], ptsY[i + 1], 6.0f, currentTheme.glow);
            canvas.drawWideLine(ptsX[i], ptsY[i], ptsX[i + 1], ptsY[i + 1], 4.0f, currentTheme.primary);
            canvas.drawWideLine(ptsX[i], ptsY[i], ptsX[i + 1], ptsY[i + 1], 1.8f, currentTheme.secondary);
        }

        // Cute dimple dots at smile ends
        canvas.fillSmoothCircle(ptsX[0], ptsY[0], 2.5f, currentTheme.accent);
        canvas.fillSmoothCircle(ptsX[NUM_SMILE_PTS - 1], ptsY[NUM_SMILE_PTS - 1], 2.5f, currentTheme.accent);
    } else if (currentEmotion == RobotEmotion::Surprised) {
        // Surprised 'O' aperture
        canvas.drawEllipse(MCX, MCY, 10, 14, currentTheme.glow);
        canvas.drawEllipse(MCX, MCY, 9, 13, currentTheme.primary);
        canvas.drawEllipse(MCX, MCY, 7, 11, currentTheme.secondary);
        canvas.fillEllipse(MCX, MCY, 5, 9, currentTheme.bg);
    } else {
        // 9-Bar Animated Voice Synthesizer Waveform
        for (int i = 0; i < 9; ++i) {
            int32_t barX = MCX + (i - 4) * 8;
            float h = voiceHeights[i];
            float topY = (float)MCY - h / 2.0f;
            float botY = (float)MCY + h / 2.0f;

            // Bar halo
            canvas.drawWideLine((float)barX, topY, (float)barX, botY, 4.0f, currentTheme.glow);
            // Main bar
            canvas.drawWideLine((float)barX, topY, (float)barX, botY, 2.5f, currentTheme.primary);
            // Tip caps
            canvas.fillCircle(barX, (int32_t)topY, 1, currentTheme.secondary);
            canvas.fillCircle(barX, (int32_t)botY, 1, currentTheme.secondary);
        }
    }
}

void RobotFace::drawCheekBlush() {
    if (blushIntensity <= 0.05f) return;

    // Left cheek: 2 angled glowing neon dashes
    float lx1 = 62.0f, ly1 = 152.0f;
    float lx2 = 72.0f, ly2 = 155.0f;
    canvas.drawWideLine(lx1, ly1, lx1 + 8.0f, ly1 - 8.0f, 3.2f, currentTheme.accent);
    canvas.drawWideLine(lx2, ly2, lx2 + 8.0f, ly2 - 8.0f, 3.2f, currentTheme.accent);

    // Right cheek: 2 angled glowing neon dashes (mirrored)
    float rx1 = SCREEN_WIDTH - 62.0f, ry1 = 152.0f;
    float rx2 = SCREEN_WIDTH - 72.0f, ry2 = 155.0f;
    canvas.drawWideLine(rx1, ry1, rx1 - 8.0f, ry1 - 8.0f, 3.2f, currentTheme.accent);
    canvas.drawWideLine(rx2, ry2, rx2 - 8.0f, ry2 - 8.0f, 3.2f, currentTheme.accent);
}

void RobotFace::drawHolographicAccents() {
    // 1. Curious: Rotating cyber data reticle
    if (currentEmotion == RobotEmotion::Curious) {
        constexpr float RCX = 262.0f, RCY = 46.0f;
        float angle = globalTime * 3.5f;

        canvas.drawArc((int32_t)RCX, (int32_t)RCY, 11, 13, angle * 57.295f, angle * 57.295f + 70.0f, currentTheme.accent);
        canvas.drawArc((int32_t)RCX, (int32_t)RCY, 11, 13, (angle + 2.09f) * 57.295f, (angle + 2.09f) * 57.295f + 70.0f, currentTheme.accent);
        canvas.drawArc((int32_t)RCX, (int32_t)RCY, 11, 13, (angle + 4.18f) * 57.295f, (angle + 4.18f) * 57.295f + 70.0f, currentTheme.accent);
        canvas.fillSmoothCircle((int32_t)RCX, (int32_t)RCY, 2, currentTheme.secondary);
    }
    // 2. Surprised: Pulsing alert beacon !
    else if (currentEmotion == RobotEmotion::Surprised) {
        constexpr float ACX = 262.0f, ACY = 44.0f;
        float scale = 1.0f + 0.15f * sinf(globalTime * 12.0f);
        canvas.drawWideLine(ACX, ACY - 8.0f * scale, ACX, ACY + 3.0f * scale, 3.5f * scale, currentTheme.accent);
        canvas.fillSmoothCircle((int32_t)ACX, (int32_t)(ACY + 8.0f * scale), (int32_t)(2.2f * scale), currentTheme.accent);
    }
    // 3. Sleepy: Drifting "Z" glyphs
    else if (currentEmotion == RobotEmotion::Sleepy) {
        float zOffset1 = fmodf(globalTime * 16.0f, 40.0f);
        float zX1 = 252.0f + sinf(zOffset1 * 0.1f) * 6.0f;
        float zY1 = 70.0f - zOffset1;

        canvas.setTextSize(1);
        canvas.setTextColor(currentTheme.accent, currentTheme.bg);
        canvas.drawString("z", (int32_t)zX1, (int32_t)zY1);

        float zOffset2 = fmodf(globalTime * 16.0f + 20.0f, 40.0f);
        float zX2 = 266.0f + sinf(zOffset2 * 0.1f) * 6.0f;
        float zY2 = 70.0f - zOffset2;
        canvas.setTextSize(2);
        canvas.drawString("Z", (int32_t)zX2, (int32_t)zY2);
    }
    // 4. Focused: Precision reticle scan line
    else if (currentEmotion == RobotEmotion::Focused) {
        float scanY = EYE_NOMINAL_Y + sinf(globalTime * 5.0f) * 16.0f;
        canvas.drawFastHLine(60, (int32_t)scanY, 200, currentTheme.glow);
        canvas.drawPixel(160, (int32_t)scanY, currentTheme.accent);
    }
}
