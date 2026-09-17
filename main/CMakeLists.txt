idf_component_register(
    SRCS "main.c" "lcd.c" "backlight.c" "nrf_link.c" "ui_port.c" "ui.c" "gps.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_gpio esp_driver_ledc esp_driver_uart esp_timer esp_app_format
)
