idf_component_register(
    SRCS "main.c" "lcd.c" "backlight.c" "nrf_link.c" "ui_port.c" "ui.c" "gps.c" "sdcard.c" "tracklog.c" "fit.c" "utc.c" "usb_msc.c" "touch.c" "ant.c" "sensor_list.c" "stats.c" "fields.c" "layouts.c" "config.c" "datapage.c" "menu.c" "devcon.c" "theme.c" "ride.c" "trip.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_gpio esp_driver_ledc esp_driver_uart esp_timer esp_app_format fatfs sdmmc esp_driver_sdmmc vfs hal esp_driver_i2c json nvs_flash esp_driver_usb_serial_jtag
)
