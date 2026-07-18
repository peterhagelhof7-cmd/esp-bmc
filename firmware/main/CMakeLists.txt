idf_component_register(
  SRCS "main.c"
  INCLUDE_DIRS "."
  # wireguard_manager wird unabhaengig von CONFIG_ESP_BMC_WG_ENABLE immer
  # mitgebaut (P1-Machbarkeits-Spike: Kompilierbarkeit + Flash-/RAM-Fussabdruck
  # sollen schon vor echter Hardware messbar sein, siehe Pflichtenheft
  # Abschnitt 12 / docs/entscheidungen.md).
  REQUIRES gpio_manager config_manager network_manager sensor_manager storage_manager usb_manager user_manager web_server_manager wireguard_manager audit_log sensor_history time_manager snmp_manager ssh_manager watchdog_manager notification_manager ota_manager firmware_version app_update
)
