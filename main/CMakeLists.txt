idf_component_register(
    SRCS "main.c" "bridge.c" "ccu_link.c" "yamaha_proto.c" "iphone_link.c" "apple_proto.c" "ota_update.c" "creds.c" "learn_mode.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash esp_timer app_update esp_app_format bootloader_support esp_http_server
                  esp_wifi esp_netif esp_event esp_driver_gpio
)
