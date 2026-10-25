idf_component_register(
	SRCS main.c littlefs_storage.c time_service.c wifi_client.c web_server.c uart_counter.c
	INCLUDE_DIRS "."
	REQUIRES esp_wifi esp_event esp_netif esp_http_server nvs_flash esp_littlefs lwip
)
