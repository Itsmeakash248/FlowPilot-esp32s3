idf_component_register(SRCS "main.cpp" "RobotFace.cpp" "RgbLed.cpp" "WifiManager.cpp" "MotorDriver.cpp" "SdCardManager.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES LovyanGFX esp_timer esp_driver_gpio esp_driver_ledc esp_driver_spi spi_flash freertos esp_hw_support espressif__led_strip esp_wifi esp_event esp_netif nvs_flash lwip hal esp_hal_pmu esp_phy fatfs esp_driver_sdspi sdmmc vfs)
