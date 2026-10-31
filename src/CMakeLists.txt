idf_component_register(
    SRCS main.c led.c sleep.c cube.c littlefs_storage.c time_service.c wifi_client.c web_server.c uart_counter.c mpu6050.c orientation_history.c
    INCLUDE_DIRS "."
    REQUIRES driver esp_hw_support esp_timer esp_wifi esp_event esp_netif esp_http_server nvs_flash esp_littlefs lwip
)
