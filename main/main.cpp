#include <stdio.h>
#include <cmath>
#include <algorithm>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "LGFX_Config.hpp"
#include "RobotFace.hpp"

static const char *TAG = "ROBOT_MAIN";

static LGFX display;

/**
 * @brief FreeRTOS task handling the high-speed 60 FPS animation loop and touch processing
 */
static void robot_face_task(void *pvParameters) {
    ESP_LOGI(TAG, "Starting RobotFace animation engine...");

    RobotFace robotFace(display);
    if (!robotFace.init()) {
        ESP_LOGE(TAG, "Failed to initialize RobotFace!");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "RobotFace initialized. Entering interactive 60 FPS loop...");

    int64_t lastTime = esp_timer_get_time();
    int64_t fpsLogTime = lastTime;
    uint32_t frameCount = 0;

    // Touch gesture tracking state
    bool wasTouching = false;
    int32_t touchStartX = 0, touchStartY = 0;
    int64_t touchStartTime = 0;

    while (true) {
        int64_t frameStart = esp_timer_get_time();
        float dt = (float)(frameStart - lastTime) / 1000000.0f;
        lastTime = frameStart;

        // Cap dt to prevent massive jumps during debug pauses
        if (dt > 0.05f) {
            dt = 0.05f;
        }

        // 1. Process XPT2046 Touch Screen Input
        int32_t touchX = 0, touchY = 0;
        bool isTouching = (display.getTouch(&touchX, &touchY) > 0);

        if (isTouching) {
            if (!wasTouching) {
                // Touch press started
                wasTouching = true;
                touchStartX = touchX;
                touchStartY = touchY;
                touchStartTime = frameStart;
            }

            // Continuous tracking (eyes follow user's finger)
            robotFace.handleTouch(touchX, touchY);
        } else {
            if (wasTouching) {
                // Touch release detected: check for tap gesture
                int64_t touchDurationMs = (frameStart - touchStartTime) / 1000;
                int32_t dx = std::abs(touchX - touchStartX);
                int32_t dy = std::abs(touchY - touchStartY);

                // Quick tap (< 350ms with minimal travel distance)
                if (touchDurationMs < 350 && dx < 25 && dy < 25) {
                    ESP_LOGI(TAG, "Screen tap detected! Cycling to next emotion...");
                    robotFace.nextEmotion();
                }

                robotFace.releaseTouch();
                wasTouching = false;
            }
        }

        // 2. Update robot kinematics, behaviors, and animations
        robotFace.update(dt);

        // 3. Render frame to double-buffer sprite and push to display
        robotFace.render();

        frameCount++;

        // Periodic FPS telemetry
        if (frameStart - fpsLogTime >= 5000000) { // Every 5 seconds
            float actualFps = (float)frameCount / ((float)(frameStart - fpsLogTime) / 1000000.0f);
            ESP_LOGI(TAG, "Display Status: running at %.1f FPS | Current Emotion: %d",
                     actualFps, (int)robotFace.getEmotion());
            fpsLogTime = frameStart;
            frameCount = 0;
        }

        // 4. Rate-limiting sleep to target ~60 FPS (16.6ms per frame)
        int64_t frameElapsed = esp_timer_get_time() - frameStart;
        int64_t targetSleepUs = 16666 - frameElapsed;

        if (targetSleepUs > 1500) {
            vTaskDelay(pdMS_TO_TICKS(targetSleepUs / 1000));
        } else {
            vTaskDelay(1); // Yield to other FreeRTOS tasks
        }
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Initializing LovyanGFX display (ESP32-S3 + ILI9341 + XPT2046)...");

    // 1. Initialize Display Controller
    if (!display.init()) {
        ESP_LOGE(TAG, "LovyanGFX display initialization failed!");
        return;
    }

    // Landscape orientation: 320 x 240
    display.setRotation(1);
    display.setColorDepth(16);
    display.setBrightness(255);
    display.fillScreen(TFT_BLACK);

    ESP_LOGI(TAG, "LovyanGFX display initialized (320x240 landscape).");

    // 2. Launch RobotFace animation task pinned to Core 1 for optimal jitter-free performance
    xTaskCreatePinnedToCore(
        robot_face_task,
        "robot_face",
        8192,
        NULL,
        5,      // Priority
        NULL,
        1       // Pinned to Core 1
    );

    ESP_LOGI(TAG, "app_main initialization complete. Animation task running.");
}
