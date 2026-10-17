#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wear_levelling.h"
#include <stdio.h>

#include "LGFX_Config.hpp"
#include "emblem_data.h"

static const char *TAG = "PNG_DISPLAY";

static LGFX display;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

static const char *FATFS_MOUNT_POINT = "/fatfs";
static const char *EMBLEM_FILE_PATH = "/fatfs/emblem.png";

static esp_err_t init_fatfs(void) {
  const esp_vfs_fat_mount_config_t mount_config = {
      .format_if_mount_failed = true,
      .max_files = 4,
      .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
      .disk_status_check_enable = false,
      .use_one_fat = false};

  esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(FATFS_MOUNT_POINT, "storage",
                                                   &mount_config, &s_wl_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount FATFS (%s)", esp_err_to_name(err));
    return err;
  }

  // Ensure emblem file exists on FATFS for drawPngFile
  FILE *f = fopen(EMBLEM_FILE_PATH, "rb");
  if (!f) {
    f = fopen(EMBLEM_FILE_PATH, "wb");
    if (f) {
      fwrite(EMBLEM_220_PNG, 1, EMBLEM_220_PNG_SIZE, f);
      fclose(f);
      ESP_LOGI(TAG, "Saved emblem PNG to %s", EMBLEM_FILE_PATH);
    } else {
      ESP_LOGE(TAG, "Failed to create %s", EMBLEM_FILE_PATH);
    }
  } else {
    fclose(f);
  }

  return ESP_OK;
}

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "Initializing LovyanGFX display...");

  // 1. Initialize display (320x240 landscape)
  if (!display.init()) {
    ESP_LOGE(TAG, "LovyanGFX init failed!");
    return;
  }

  display.setRotation(1);
  display.setColorDepth(16);
  display.setBrightness(255);
  display.fillScreen(TFT_BLACK);

  // 2. Initialize FATFS filesystem
  esp_err_t fs_status = init_fatfs();

  // 3. Display PNG
  bool drawn = false;
  if (fs_status == ESP_OK) {
    ESP_LOGI(TAG, "Drawing PNG via drawPngFile()...");
    drawn = display.drawPngFile(EMBLEM_FILE_PATH, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f,
                                datum_t::middle_center);
  }

  // Fallback to memory decode if filesystem decode is unavailable
  if (!drawn) {
    ESP_LOGW(TAG, "Falling back to drawPng() from memory...");
    drawn = display.drawPng(EMBLEM_220_PNG, EMBLEM_220_PNG_SIZE, 0, 0, 0, 0, 0,
                            0, 1.0f, 1.0f, datum_t::middle_center);
  }

  ESP_LOGI(TAG, "PNG display %s", drawn ? "successful" : "failed");

  // Keep the task alive
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
