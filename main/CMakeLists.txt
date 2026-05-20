idf_component_register(
    SRCS "main.c" "motor.c" "web.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES "driver" "tmc2208" "esp_wifi" "esp_event" "esp_netif" "nvs_flash" "esp_http_server"
)
