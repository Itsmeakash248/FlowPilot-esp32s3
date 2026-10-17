idf_component_register(SRCS "main.cpp" "RobotFace.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES LovyanGFX esp_timer esp_driver_gpio esp_driver_spi spi_flash freertos esp_hw_support)
