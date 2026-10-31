idf_component_register(
    SRCS "main.c" "lcd.c" "backlight.c" "nrf_link.c" "ui_port.c" "ui.c" "gps.c" "sdcard.c" "tracklog.c" "fit.c" "utc.c" "usb_msc.c" "touch.c" "ant.c" "sensor_list.c" "stats.c" "fields.c" "layouts.c" "config.c" "datapage.c" "menu.c" "devcon.c" "theme.c" "ride.c" "trip.c" "mapfile.c" "mapview.c" "route.c" "trail.c" "sun.c" "fitread.c" "history.c" "health.c" "shifting.c" "dbglog.c" "wifi_aid.c" "gps_aid_protocol.c"
         "map_header.c" "agnss.c" "epo.c" "l10n.c" "ui_fonts.c" "font_cyrillic_8.c" "font_cyrillic_14.c"
         "font_cyrillic_20.c" "font_cyrillic_28.c" "font_cyrillic_48.c"
    PRIV_REQUIRES esp_http_client mbedtls
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_gpio esp_driver_ledc esp_driver_uart esp_timer esp_app_format fatfs sdmmc esp_driver_sdmmc vfs hal esp_driver_i2c json nvs_flash esp_driver_usb_serial_jtag esp_wifi esp_event esp_netif lwip
)

# The map reader and rasteriser are the hot loops of a map render: -O2 there
# (the rest of the firmware stays -Os).
set_source_files_properties(mapfile.c mapview.c PROPERTIES COMPILE_OPTIONS "-O2")
