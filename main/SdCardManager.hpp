#pragma once

#include <cstdint>
#include <string>
#include <sys/stat.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

/**
 * @brief SD Card Configuration according to FlxOS Profile: ESP32-S3 + ILI9341 + XPT2046
 *
 * Profile ID: esp32s3-ili9341-xpt
 * Hardware:
 *   - Bus: Shared SPI2_HOST (FSPI)
 *   - SCLK: GPIO 12
 *   - MOSI: GPIO 11
 *   - MISO: GPIO 13
 *   - CS:   GPIO 4
 *   - Mount Point: "/sdcard"
 *   - Max Frequency: 20000 kHz (20 MHz)
 */
struct SdCardConfig {
    static constexpr const char* DEFAULT_MOUNT_POINT = "/sdcard";
    static constexpr gpio_num_t DEFAULT_CS_PIN = GPIO_NUM_4;

    gpio_num_t csPin = DEFAULT_CS_PIN;      ///< SD Card SPI Chip Select pin (GPIO 4)
    gpio_num_t mosiPin = GPIO_NUM_11;       ///< Shared SPI MOSI pin (GPIO 11)
    gpio_num_t misoPin = GPIO_NUM_13;       ///< Shared SPI MISO pin (GPIO 13)
    gpio_num_t sclkPin = GPIO_NUM_12;       ///< Shared SPI SCLK pin (GPIO 12)

    spi_host_device_t spiHost = SPI2_HOST;  ///< SPI Host (SPI2_HOST = host 2 on ESP32-S3)
    uint32_t maxFreqKhz = 20000;            ///< Maximum SPI frequency in kHz (20 MHz)
    std::string mountPoint = DEFAULT_MOUNT_POINT; ///< VFS Mount path
    int maxFiles = 5;                       ///< Max simultaneous open files
    bool formatOnFail = false;              ///< Format filesystem if mounting fails
    bool sharedBus = true;                  ///< Set to true when sharing SPI bus with Display/Touch
};

/**
 * @brief High-level telemetry & status information for the SD card
 */
struct SdCardInfo {
    bool isMounted = false;                 ///< True if SD card is mounted and accessible
    char name[8] = {0};                     ///< Card product name (e.g. "SD32G")
    uint32_t speedKhz = 0;                  ///< Operating frequency in kHz
    uint64_t totalBytes = 0;                ///< Total capacity in bytes
    uint64_t freeBytes = 0;                 ///< Free space in bytes
    uint32_t sectorSize = 0;                ///< Sector size in bytes (usually 512)
};

/**
 * @brief Thread-safe SD Card Manager handling mounting, unmounting, status,
 *        and POSIX filesystem utilities on a shared SPI bus.
 */
class SdCardManager {
public:
    static SdCardManager& getInstance();

    /**
     * @brief Configure pins and attempt to mount SD Card
     * @param config Optional custom configuration; defaults to FlxOS profile specs
     * @return esp_err_t ESP_OK on success, or appropriate error code if card is missing or unreadable
     */
    esp_err_t init(const SdCardConfig& config = SdCardConfig());

    /**
     * @brief Safely unmount SD Card and release filesystem resources
     * @return esp_err_t ESP_OK on success
     */
    esp_err_t unmount();

    /**
     * @brief Check if SD card is currently mounted and accessible
     */
    bool isMounted() const { return mounted; }

    /**
     * @brief Get telemetry & partition space details
     */
    SdCardInfo getInfo();

    /**
     * @brief Get configured mount point (e.g. "/sdcard")
     */
    const std::string& getMountPoint() const { return currentConfig.mountPoint; }

    /**
     * @brief Check if a file exists on the SD card
     * @param path Relative path from mount point or full path (e.g. "/sdcard/test.txt" or "test.txt")
     */
    bool fileExists(const std::string& path) const;

    /**
     * @brief Perform quick read/write self-test on the SD card
     * @return true if self-test passes, false on error
     */
    bool testCard();

    /**
     * @brief Format SD card with FAT filesystem
     */
    esp_err_t format();

    /**
     * @brief Print SD card diagnostics to log output
     */
    void printCardInfo();

private:
    SdCardManager();
    ~SdCardManager();
    SdCardManager(const SdCardManager&) = delete;
    SdCardManager& operator=(const SdCardManager&) = delete;

    SdCardConfig currentConfig;
    sdmmc_card_t* cardHandle;
    bool mounted;
};

/**
 * @brief Convenience global accessor
 */
inline SdCardManager& getSdCardManager() {
    return SdCardManager::getInstance();
}
