idf_component_register(
  SRCS "usb_manager.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_driver_usb_serial_jtag storage_manager audit_log esp_timer gpio_manager network_manager
           sensor_history sensor_manager user_manager wireguard_manager config_manager snmp_manager
)
