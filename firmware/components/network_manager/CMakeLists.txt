idf_component_register(
  SRCS "network_manager.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip time_manager storage_manager espressif__cjson
)
