idf_component_register(
  SRCS "web_server_manager.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_http_server gpio_manager network_manager sensor_manager usb_manager user_manager wireguard_manager esp_timer audit_log sensor_history config_manager snmp_manager ssh_manager notification_manager ota_manager esp_system
)
