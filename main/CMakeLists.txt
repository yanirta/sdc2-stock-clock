idf_component_register(
    SRCS main.c settings.c gfx.c fonts.c display.c quotes.c wifi.c web.c buzzer.c
    INCLUDE_DIRS "."
    EMBED_TXTFILES index.html
    PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_ledc esp_driver_gpio esp_http_client esp_http_server
                  esp_wifi esp_netif esp_event nvs_flash app_update esp_partition esp_timer mbedtls esp_app_format spi_flash)
