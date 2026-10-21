#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cmath>
#include <stdio.h>

#include "LGFX_Config.hpp"
#include "MotorDriver.hpp"
#include "PerfMonitor.hpp"
#include "RgbLed.hpp"
#include "RobotFace.hpp"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_phy_init.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <esp_private/brownout.h>
#include <esp_rom_sys.h>

/**
 * @brief Subsystem feature toggles (0 = disabled, 1 = enabled)
 */
#define ENABLE_WIFI 0              ///< Wi-Fi station mode toggle (0 = disabled, 1 = enabled)
#define ENABLE_ROBOT_SUBSYSTEMS 1  ///< Robot subsystems (display, motors, LEDs, touch, SMP tasks)
#define ENABLE_MOTOR_TEST 0              ///< Dual-motor hardware self-test suite toggle (0 = disabled, 1 = enabled)
#define ENABLE_RGB_LED 0                 ///< Onboard WS2812 NeoPixel RGB LED on GPIO 48 (0 = disabled, 1 = enabled)

#if ENABLE_MOTOR_TEST
#include "MotorTest.hpp"
#endif

/**
 * @brief Hardware & configuration constants
 */
#if ENABLE_RGB_LED
static constexpr uint8_t RGB_LED_MAX_BRIGHTNESS =
    RgbLed::MAX_BRIGHTNESS_LIMIT; ///< Safe hardware limit capped at 20% (51 / 255
                                  ///< = 20.0%)
#endif
static const char *TAG = "ROBOT_MAIN";

static LGFX display;
#if ENABLE_RGB_LED
static RgbLed rgbLed;
#endif

static const char *getEmotionName(RobotEmotion emo) {
  switch (emo) {
  case RobotEmotion::Neutral:   return "Neutral";
  case RobotEmotion::Happy:     return "Happy";
  case RobotEmotion::Curious:   return "Curious";
  case RobotEmotion::Surprised: return "Surprised";
  case RobotEmotion::Focused:   return "Focused";
  case RobotEmotion::Sleepy:    return "Sleepy";
  case RobotEmotion::Wink:      return "Wink";
  case RobotEmotion::Sad:       return "Sad";
  case RobotEmotion::Rage:      return "Rage";
  case RobotEmotion::Dasara:    return "Happy Dasara";
  default:                      return "Unknown";
  }
}

static RobotFace robotFace(display);

/**
 * @brief Core 0 Task: Robot Supervisor, Autonomous AI, Kinematics, Motor Slew, LEDs & Telemetry (100 Hz)
 */
static void robot_control_task(void *pvParameters) {
  ESP_LOGI(TAG, "Starting FlowPilot Robot Control Engine on Core 0 (PRO_CPU)...");

  int64_t lastTime = esp_timer_get_time();
  int64_t fpsLogTime = lastTime;

  while (true) {
    int64_t loopStart = esp_timer_get_time();
    float dt = (float)(loopStart - lastTime) / 1000000.0f;
    lastTime = loopStart;

    // Cap dt to prevent massive jumps during debug pauses
    if (dt > 0.05f) {
      dt = 0.05f;
    }

    // 1. Update performance telemetry monitor
    PerfMonitor &perf = PerfMonitor::getInstance();
    perf.update(dt);
    robotFace.setPerfStats(perf.getFps(), perf.getCpuCore1(), perf.getCpuCore2(),
                           perf.getCpuTotal(), perf.getFreeHeap());

    // 2. Update robot kinematics, autonomous behaviors, and motor driver slew rates
    robotFace.update(dt);
    getMotorDriver().update(dt);

    // 3. Update Onboard RGB LED (GPIO 48)
#if ENABLE_RGB_LED
    rgbLed.syncWithFace(robotFace);
    rgbLed.update(dt);
#endif


    // 4. Periodic FPS & CPU telemetry logging every 5 seconds
    if (loopStart - fpsLogTime >= 5000000) {
      float actualFps = perf.getFps();
      const MotorTelemetry &motorTelem = getMotorDriver().getTelemetry();
#if ENABLE_MOTOR_TEST
      const char *motorPhase = getMotorTestPhaseName();
#else
      const char *motorPhase = "Disabled";
#endif
      ESP_LOGI(TAG,
               "Display Status: %.1f FPS | Core 1: %.1f%% | Core 2: %.1f%% (Total: %.1f%%) | Heap: %u KB | Emotion: %s (%d) | Motors: L: %+.2f R: %+.2f (%s)",
               actualFps, perf.getCpuCore1(), perf.getCpuCore2(), perf.getCpuTotal(),
               (unsigned int)(perf.getFreeHeap() / 1024),
               getEmotionName(robotFace.getEmotion()),
               (int)robotFace.getEmotion(),
               motorTelem.currentLeft, motorTelem.currentRight, motorPhase);
      fpsLogTime = loopStart;
    }

    // 5. Cadence control: Maintain crisp 100 Hz control loop (10ms)
    int64_t elapsedUs = esp_timer_get_time() - loopStart;
    int64_t targetSleepUs = 10000 - elapsedUs;

    if (targetSleepUs > 1500) {
      vTaskDelay(pdMS_TO_TICKS(targetSleepUs / 1000));
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

/**
 * @brief Core 1 Task: Dedicated High-Speed Graphics Rasterizer, SPI Display Push & Touch Processing
 */
static void robot_render_task(void *pvParameters) {
  ESP_LOGI(TAG, "Starting RobotFace graphics rendering & touch engine on Core 1 (APP_CPU)...");

  // Touch gesture tracking state
  bool wasTouching = false;
  int32_t touchStartX = 0, touchStartY = 0;
  int64_t touchStartTime = 0;

  while (true) {
    int64_t frameStart = esp_timer_get_time();

    // 1. Process XPT2046 Touch Screen Input (Hardware SPI2 managed exclusively on Core 1)
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
          if (robotFace.isTouchOnOverlay(touchStartX, touchStartY)) {
            robotFace.cyclePerfOverlayMode();
            ESP_LOGI(TAG, "Touch on overlay detected! Cycled PerfOverlayMode to %d",
                     (int)robotFace.getPerfOverlayMode());
          } else {
            ESP_LOGI(TAG, "Screen tap detected! Cycling to next emotion...");
            robotFace.nextEmotion();
          }
        }

        robotFace.releaseTouch();
        wasTouching = false;
      }
    }

    // 2. Render frame to double-buffer sprite and push to display via SPI
    robotFace.render();
    PerfMonitor::getInstance().onFrameRendered();

    // 3. Dynamic Frame Pacing & Cooperative FreeRTOS Yielding
    // Targets ~30 FPS with guaranteed cooperative idle time so Core 1 is balanced
    int64_t frameElapsed = esp_timer_get_time() - frameStart;
    int64_t targetSleepUs = 33333 - frameElapsed;

    if (targetSleepUs > 2000) {
      vTaskDelay(pdMS_TO_TICKS(targetSleepUs / 1000));
    } else {
      // Cooperative yield: ensure IDLE task on Core 1 gets at least 3ms every frame
      vTaskDelay(pdMS_TO_TICKS(3));
    }
  }
}

#if ENABLE_WIFI
/**
 * @brief Wi-Fi Station Configuration & Event Handling (Official ESP-IDF Station Flow)
 */
#define WIFI_SSID "realme NARZO 70 Turbo 5G"
#define WIFI_PASS "Trebledroids"

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGI("WIFI_STA", "Disconnected. Reconnecting to AP...");
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI("WIFI_STA", "Connected! Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
  }
}

static void wifi_init_sta(void) {
  // Let power rails settle after boot
  vTaskDelay(pdMS_TO_TICKS(300));

  // Initialize NVS (required by Wi-Fi driver)
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  cfg.sta_disconnected_pm = false; // Prevent modem-sleep from gating USB clock when disconnected
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

  wifi_config_t wifi_config = {};
  strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
  strcpy((char *)wifi_config.sta.password, WIFI_PASS);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_set_ps(WIFI_PS_NONE); // Disable power save modem sleep
  esp_wifi_set_max_tx_power(40); // 10 dBm safe minimum (40 * 0.25 dBm) to prevent USB brownouts

  ESP_LOGI("WIFI_STA", "Wi-Fi STA initialized. Connecting to \"%s\"...", WIFI_SSID);
}
#endif

extern "C" void app_main(void) {
  // Disable brownout detector to prevent false-positive software resets during
  // RF startup on USB power
  esp_brownout_disable();

  esp_reset_reason_t rstReason = esp_reset_reason();
  ESP_LOGI(TAG, "=== FlowPilot-esp32s3 [Happy Dasara Edition ✨] Starting | Reset Reason: %d ===",
           (int)rstReason);

  // Initialize Performance & CPU telemetry engine
  PerfMonitor::getInstance().init();

#if ENABLE_WIFI
  // Initialize Wi-Fi station early before heavy peripherals (display, motors, LEDs)
  wifi_init_sta();
#else
  ESP_LOGI(TAG, "Wi-Fi subsystem disabled (ENABLE_WIFI=0).");
#endif

#if ENABLE_ROBOT_SUBSYSTEMS
  // 1. Deselect display and touch chip selects (held inactive HIGH during bus
  // initialization).
  gpio_config_t csConf = {
      .pin_bit_mask = (1ULL << GPIO_NUM_5) | (1ULL << GPIO_NUM_10),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&csConf);
  gpio_set_level(GPIO_NUM_5, 1);  // Touch CS inactive
  gpio_set_level(GPIO_NUM_10, 1); // Display CS inactive

  // 2. Initialize LovyanGFX Display Controller (ILI9341 + XPT2046 on SPI2)
  ESP_LOGI(TAG,
           "Initializing LovyanGFX display (ESP32-S3 + ILI9341 + XPT2046)...");
  if (!display.init()) {
    ESP_LOGE(TAG, "LovyanGFX display initialization failed!");
    return;
  }

  // Release bus lock left open by LovyanGFX display init (matching FlxOS
  // LgfxDisplayDevice.cpp:92)
  display.endWrite();

  // Landscape orientation: 320 x 240
  display.setRotation(1);
  display.setColorDepth(16);
  display.setBrightness(
      200); // 200/255 provides bright visuals with low current draw
  display.fillScreen(TFT_BLACK);
  display.endWrite();

  ESP_LOGI(TAG, "LovyanGFX display initialized (320x240 landscape).");

  // 2b. Initialize RobotFace Engine
  if (!robotFace.init()) {
    ESP_LOGE(TAG, "Failed to initialize RobotFace!");
    return;
  }
  ESP_LOGI(TAG, "RobotFace initialized successfully.");

  // 3. Initialize Onboard WS2812 RGB LED on GPIO 48
#if ENABLE_RGB_LED
  esp_err_t ledErr = rgbLed.init(GPIO_NUM_48);
  if (ledErr == ESP_OK) {
    rgbLed.setBrightness(RGB_LED_MAX_BRIGHTNESS);
    rgbLed.setMode(RgbLedMode::EmotionSync);
    ESP_LOGI(TAG,
             "Onboard WS2812 RGB LED (GPIO 48) initialized in EmotionSync mode "
             "(Brightness: %u/255, Limit: %u/255 [20%%]).",
             rgbLed.getBrightness(), RGB_LED_MAX_BRIGHTNESS);
  } else {
    ESP_LOGW(TAG, "Onboard RGB LED initialization returned: %s",
             esp_err_to_name(ledErr));
  }
#else
  // Hold the WS2812 data line low so the NeoPixel stays dark
  gpio_reset_pin(GPIO_NUM_48);
  gpio_set_direction(GPIO_NUM_48, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_48, 0);
  ESP_LOGI(TAG, "Onboard RGB LED subsystem disabled (ENABLE_RGB_LED=0), "
                "GPIO 48 held LOW.");
#endif


  // 4. Initialize L298N / L829 Dual H-Bridge Motor Driver (4-wire direct PWM,
  // ENA/ENB excluded)
  esp_err_t motorErr = getMotorDriver().init();
  if (motorErr == ESP_OK) {
    ESP_LOGI(TAG, "L298N / L829 Motor Driver initialized (4-wire direct PWM: "
                  "IN1=15, IN2=16 | IN3=18, IN4=17 | ENA/ENB excluded).");
#if ENABLE_MOTOR_TEST
    ESP_LOGI(TAG, "Starting FlowPilot Dual Motor Hardware Self-Test Suite on Core 0...");
    startMotorTestTask();
#endif
  } else {
    ESP_LOGE(TAG, "Motor driver initialization returned: %s",
             esp_err_to_name(motorErr));
  }


  // 5. Launch Core 0: High-speed Control, Kinematics & Hardware Supervisor Task (100 Hz)
  xTaskCreatePinnedToCore(robot_control_task, "robot_ctrl", 4096, NULL,
                          4, // Priority
                          NULL,
                          0 // Pinned to Core 0 (PRO_CPU)
  );

  // 6. Launch Core 1: Dedicated Graphics Rasterizer, SPI Display Push & Touch Pipeline
  xTaskCreatePinnedToCore(robot_render_task, "robot_render", 8192, NULL,
                          5, // Priority
                          NULL,
                          1 // Pinned to Core 1 (APP_CPU)
  );

  ESP_LOGI(TAG,
           "app_main complete: Dual-Core SMP Active (Core 0: Control/Kinematics, Core 1: Render/Touch).");
#else
  // Keep LCD backlight pin LOW to prevent current draw
  gpio_reset_pin(GPIO_NUM_7);
  gpio_set_direction(GPIO_NUM_7, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_7, 0);

  ESP_LOGI(TAG, "=== All robot subsystems disabled: Pure Wi-Fi Station Mode Active ===");
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "Wi-Fi Standalone Heartbeat | Free Heap: %lu B", (unsigned long)esp_get_free_heap_size());
  }
#endif
}
