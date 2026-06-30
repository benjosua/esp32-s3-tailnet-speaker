idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        microlink
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_http_server
        json
        driver
        espressif__led_strip
        espressif__esp_codec_dev
)
