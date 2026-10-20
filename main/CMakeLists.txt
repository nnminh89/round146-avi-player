idf_component_register(
    SRCS "main.cpp" "board.cpp" "video.cpp" "audio.cpp" "ui.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ledc
        esp_driver_sdmmc
        esp_driver_spi
        esp_lcd
        fatfs
        nvs_flash
        sdmmc
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Wshadow -Wno-missing-field-initializers)
