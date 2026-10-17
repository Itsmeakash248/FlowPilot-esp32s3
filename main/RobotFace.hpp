#pragma once

#include <cstdint>
#include <cmath>
#include "LGFX_Config.hpp"

/**
 * @brief Robot facial emotional states
 */
enum class RobotEmotion {
    Neutral,     ///< Calm, observant cybernetic companion
    Happy,       //< Joyful arch eyes, glowing blush, bouncy smile waveform
    Curious,     //< Cocked eyebrow, wandering gaze, holographic data ring
    Surprised,   //< Wide round eyes, dilated pupils, alert beacon
    Focused,     //< Determined angled eyes, low sharp brow, scanning reticle
    Sleepy,      //< Heavy drooping eyelids, slow deep breathing, floating "Z"s
    Wink,        //< Playful wink with starburst sparkle
    Sad,         //< Drooping outer lids, low brows, soft melancholy blue
    Rage,        //< Fiery crimson eyes, intense downward slant, fast pulse
    COUNT
};

/**
 * @brief 16-bit RGB565 Cybernetic Color Palette for each emotion
 */
struct RobotTheme {
    uint16_t primary;     ///< Main vibrant glow color (iris, brows, mouth)
    uint16_t secondary;   ///< Bright highlight / white-hot core
    uint16_t glow;        ///< Soft ambient halo / bloom
    uint16_t accent;      ///< Secondary details (blush, holographic icons)
    uint16_t bg;          ///< Obsidian visor background
};

/**
 * @brief Detailed Animated Robot Face Engine for LovyanGFX
 */
class RobotFace {
public:
    explicit RobotFace(LGFX& display);
    ~RobotFace();

    bool init();
    void update(float dt);
    void render();

    // Emotion controls
    void setEmotion(RobotEmotion emotion, bool immediate = false);
    RobotEmotion getEmotion() const { return currentEmotion; }
    void nextEmotion();

    // User interaction
    void handleTouch(int32_t x, int32_t y);
    void releaseTouch();
    void triggerBlink();
    void triggerWink();
    void setLookAt(float normX, float normY); // -1.0f to 1.0f

private:
    LGFX& display;
    LGFX_Sprite canvas;
    bool canvasReady;

    // Emotions
    RobotEmotion currentEmotion;
    RobotEmotion targetEmotion;

    // Interpolated Color Theme
    RobotTheme currentTheme;
    RobotTheme targetTheme;
    float themeTransition;

    // Display constants
    static constexpr int32_t SCREEN_WIDTH = 320;
    static constexpr int32_t SCREEN_HEIGHT = 240;

    static constexpr float EYE_CENTER_LEFT_X  = 100.0f;
    static constexpr float EYE_CENTER_RIGHT_X = 220.0f;
    static constexpr float EYE_NOMINAL_Y      = 108.0f;
    static constexpr float EYE_BASE_WIDTH     = 70.0f;
    static constexpr float EYE_BASE_HEIGHT    = 82.0f;
    static constexpr float EYE_BASE_RADIUS    = 24.0f;

    // Kinematics & Physical Parameters
    float gazeX, targetGazeX;             // -26.0 to +26.0 px
    float gazeY, targetGazeY;             // -20.0 to +20.0 px
    float eyeWidth, targetEyeWidth;
    float eyeHeight, targetEyeHeight;
    float eyeRadius, targetEyeRadius;

    // Eyelid Parameters
    float upperLidOpen, targetUpperLidOpen; // 0.0 (closed) to 1.0 (fully open)
    float lowerLidOpen, targetLowerLidOpen; // 0.0 (squinted) to 1.0 (open)
    float lidAngle, targetLidAngle;         // Eyelid tilt in radians

    // Eyebrows
    float leftBrowY, targetLeftBrowY;
    float rightBrowY, targetRightBrowY;
    float leftBrowAngle, targetLeftBrowAngle;
    float rightBrowAngle, targetRightBrowAngle;

    // Blinking State Machine
    enum class BlinkPhase { Idle, Closing, Closed, Opening };
    BlinkPhase blinkPhase;
    float blinkTimer;
    float blinkProgress;
    float nextBlinkInterval;
    bool isDoubleBlink;
    bool isWinking;

    // Autonomous Lifelike Behavior
    float saccadeTimer;
    float nextSaccadeInterval;
    float autoEmotionTimer;
    float nextAutoEmotionInterval;
    float touchReleaseTimer;
    bool isUserTouching;
    bool isBeingPetted;

    // Micro-motion & Ambient Waves
    float globalTime;
    float breathPhase;
    float bounceOffset;
    float voicePhase;
    float voiceHeights[9];
    float blushIntensity, targetBlushIntensity;

    // Internal update methods
    void updateAutonomousAI(float dt);
    void updateBlinking(float dt);
    void updateTheme(float dt);
    void updateKinematics(float dt);
    void applyEmotionPreset(RobotEmotion emotion);

    // Drawing methods
    void drawBackground();
    void drawVisorHUD();
    void drawEye(float cx, float cy, bool isLeft, float upperOpen, float lowerOpen, float slant);
    void drawHappyEye(float cx, float cy, bool isLeft);
    void drawWinkEye(float cx, float cy, bool isLeft);
    void drawEyebrow(float cx, float cy, float angle, float length);
    void drawMouth();
    void drawCheekBlush();
    void drawHolographicAccents();

    // Color helpers
    static RobotTheme getEmotionTheme(RobotEmotion emotion);
    static uint16_t blendColor(uint16_t c1, uint16_t c2, float t);
};
