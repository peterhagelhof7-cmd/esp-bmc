idf_component_register(
  SRCS "snmp_manager.c"
  INCLUDE_DIRS "include"
  REQUIRES lwip storage_manager espressif__cjson gpio_manager network_manager sensor_manager wireguard_manager
           audit_log config_manager ota_manager
)
