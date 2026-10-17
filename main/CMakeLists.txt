idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES LovyanGFX vfs fatfs wear_levelling sdmmc esp_timer esp_driver_gpio esp_driver_spi spi_flash freertos esp_hw_support)
