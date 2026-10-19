#include "SdCardManager.hpp"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include "esp_log.h"

static const char* TAG = "SD_CARD";

SdCardManager& SdCardManager::getInstance() {
    static SdCardManager instance;
    return instance;
}

SdCardManager::SdCardManager()
    : cardHandle(nullptr)
    , mounted(false)
{
}

SdCardManager::~SdCardManager() {
    if (mounted) {
        unmount();
    }
}

esp_err_t SdCardManager::init(const SdCardConfig& config) {
    currentConfig = config;

    if (mounted) {
        ESP_LOGW(TAG, "SD Card already mounted at '%s'", currentConfig.mountPoint.c_str());
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SD Card on SPI2 (CS: GPIO %d, MOSI: %d, MISO: %d, SCLK: %d, Speed: %lu kHz, Mount: '%s')...",
             (int)config.csPin, (int)config.mosiPin, (int)config.misoPin, (int)config.sclkPin,
             (unsigned long)config.maxFreqKhz, config.mountPoint.c_str());

    // 1. Enable internal pull-ups on MISO and MOSI for signal stability on shared bus
    gpio_set_pull_mode(config.misoPin, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(config.mosiPin, GPIO_PULLUP_ONLY);

    // 2. Initialize SPI bus (matches FlxOS SpiSdCardDevice: 16KB DMA transfer size)
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = config.mosiPin;
    bus_cfg.miso_io_num = config.misoPin;
    bus_cfg.sclk_io_num = config.sclkPin;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 16 * 1024;

    bool busOwner = false;
    esp_err_t busErr = spi_bus_initialize(config.spiHost, &bus_cfg, SPI_DMA_CH_AUTO);
    if (busErr == ESP_OK) {
        ESP_LOGI(TAG, "SPI bus %d initialized successfully by SD Card (16KB DMA).", (int)config.spiHost);
        busOwner = true;
    } else if (busErr == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SPI bus %d already initialized (shared with Display/Touch).", (int)config.spiHost);
        busOwner = false;
    } else {
        ESP_LOGE(TAG, "Failed to initialize SPI bus %d: %s", (int)config.spiHost, esp_err_to_name(busErr));
        return busErr;
    }

    // 3. Configure SDSPI host parameters
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = config.spiHost;
    host.max_freq_khz = config.maxFreqKhz;

    // 4. Configure SDSPI device parameters (let SDSPI driver own and configure CS pin)
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = config.csPin;
    slot_config.host_id = config.spiHost;

    // 5. Mount configuration for VFS FAT
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = config.formatOnFail,
        .max_files = config.maxFiles,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false
    };

    esp_err_t ret = esp_vfs_fat_sdspi_mount(
        config.mountPoint.c_str(),
        &host,
        &slot_config,
        &mount_config,
        &cardHandle
    );

    // If 20 MHz fails with timeout/invalid response, retry at 4000 kHz (FlxOS standard base frequency)
    if (ret != ESP_OK && config.maxFreqKhz > 4000) {
        ESP_LOGW(TAG, "Mount at %lu kHz returned %s; retrying at 4000 kHz...", (unsigned long)config.maxFreqKhz, esp_err_to_name(ret));
        host.max_freq_khz = 4000;
        ret = esp_vfs_fat_sdspi_mount(
            config.mountPoint.c_str(),
            &host,
            &slot_config,
            &mount_config,
            &cardHandle
        );
    }

    if (ret != ESP_OK) {
        if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "SD card timed out (0x107). Check if micro-SD card is inserted and securely seated.");
        } else if (ret == ESP_ERR_INVALID_RESPONSE) {
            ESP_LOGW(TAG, "No SD card response (0x108). Card is either absent, not seated, or unformatted FAT32.");
        } else if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "Failed to mount filesystem on SD card (unformatted or unsupported partition).");
        } else {
            ESP_LOGW(TAG, "SD card mount failed: %s (0x%x)", esp_err_to_name(ret), ret);
        }
        cardHandle = nullptr;
        mounted = false;

        // If SD card initialization failed and we were the SPI bus owner, free it so LovyanGFX can cleanly initialize
        if (busOwner) {
            spi_bus_free(config.spiHost);
        }
        return ret;
    }

    mounted = true;
    ESP_LOGI(TAG, "SD Card mounted successfully at '%s'!", config.mountPoint.c_str());
    printCardInfo();

    return ESP_OK;
}

esp_err_t SdCardManager::unmount() {
    if (!mounted || cardHandle == nullptr) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Unmounting SD Card from '%s'...", currentConfig.mountPoint.c_str());
    esp_err_t err = esp_vfs_fat_sdcard_unmount(currentConfig.mountPoint.c_str(), cardHandle);
    if (err == ESP_OK) {
        cardHandle = nullptr;
        mounted = false;
        ESP_LOGI(TAG, "SD Card successfully unmounted.");
    } else {
        ESP_LOGE(TAG, "Failed to unmount SD Card: %s", esp_err_to_name(err));
    }
    return err;
}

SdCardInfo SdCardManager::getInfo() {
    SdCardInfo info;
    info.isMounted = mounted;
    if (!mounted || cardHandle == nullptr) {
        return info;
    }

    // Copy card product name from CID
    snprintf(info.name, sizeof(info.name), "%.*s", (int)sizeof(cardHandle->cid.name), cardHandle->cid.name);

    info.speedKhz = cardHandle->real_freq_khz;
    info.sectorSize = cardHandle->csd.sector_size;

    // Get filesystem partition capacity and free space
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    esp_err_t err = esp_vfs_fat_info(currentConfig.mountPoint.c_str(), &totalBytes, &freeBytes);
    if (err == ESP_OK) {
        info.totalBytes = totalBytes;
        info.freeBytes = freeBytes;
    } else {
        info.totalBytes = ((uint64_t)cardHandle->csd.capacity) * cardHandle->csd.sector_size;
        info.freeBytes = 0;
    }

    return info;
}

bool SdCardManager::fileExists(const std::string& path) const {
    if (!mounted) {
        return false;
    }

    std::string fullPath;
    if (path.rfind(currentConfig.mountPoint, 0) == 0) {
        fullPath = path;
    } else {
        fullPath = currentConfig.mountPoint;
        if (!path.empty() && path[0] != '/') {
            fullPath += "/";
        }
        fullPath += path;
    }

    struct stat st;
    return (stat(fullPath.c_str(), &st) == 0);
}

bool SdCardManager::testCard() {
    if (!mounted) {
        ESP_LOGE(TAG, "Cannot test: SD Card is not mounted.");
        return false;
    }

    std::string testPath = currentConfig.mountPoint + "/flowpilot_test.txt";
    ESP_LOGI(TAG, "Running SD card read/write verification test at '%s'...", testPath.c_str());

    const char* testPayload = "FlowPilot-ESP32S3 SD Card Verification: OK!\n";
    FILE* f = fopen(testPath.c_str(), "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open '%s' for writing: errno %d (%s)", testPath.c_str(), errno, strerror(errno));
        return false;
    }
    fputs(testPayload, f);
    fclose(f);

    // Read back and verify payload
    f = fopen(testPath.c_str(), "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open '%s' for reading: errno %d (%s)", testPath.c_str(), errno, strerror(errno));
        return false;
    }
    char buffer[64] = {0};
    if (fgets(buffer, sizeof(buffer), f) == nullptr) {
        ESP_LOGE(TAG, "Failed to read test verification file.");
        fclose(f);
        return false;
    }
    fclose(f);

    if (strncmp(buffer, testPayload, strlen(testPayload)) == 0) {
        ESP_LOGI(TAG, "SD Card read/write test PASSED! Verification payload matched.");
        return true;
    } else {
        ESP_LOGE(TAG, "SD Card verification MISMATCH! Expected: '%s', Got: '%s'", testPayload, buffer);
        return false;
    }
}

esp_err_t SdCardManager::format() {
    if (!mounted || cardHandle == nullptr) {
        ESP_LOGE(TAG, "Cannot format: SD Card is not mounted.");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Formatting SD Card at '%s'...", currentConfig.mountPoint.c_str());
    esp_err_t err = esp_vfs_fat_sdcard_format(currentConfig.mountPoint.c_str(), cardHandle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SD Card successfully formatted.");
    } else {
        ESP_LOGE(TAG, "Failed to format SD Card: %s", esp_err_to_name(err));
    }
    return err;
}

void SdCardManager::printCardInfo() {
    if (!mounted || cardHandle == nullptr) {
        ESP_LOGW(TAG, "SD Card is not mounted.");
        return;
    }

    sdmmc_card_print_info(stdout, cardHandle);

    uint64_t totalBytes = 0, freeBytes = 0;
    if (esp_vfs_fat_info(currentConfig.mountPoint.c_str(), &totalBytes, &freeBytes) == ESP_OK) {
        float totalMb = (float)totalBytes / (1024.0f * 1024.0f);
        float freeMb = (float)freeBytes / (1024.0f * 1024.0f);
        float usedMb = totalMb - freeMb;
        ESP_LOGI(TAG, "Filesystem Capacity: %.2f MB total | %.2f MB used | %.2f MB free (%.1f%% available)",
                 totalMb, usedMb, freeMb, (totalBytes > 0) ? (freeMb / totalMb * 100.0f) : 0.0f);
    }
}
