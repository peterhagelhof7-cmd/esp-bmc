idf_component_register(
  SRCS "notification_manager.c"
  INCLUDE_DIRS "include"
  REQUIRES espressif__cjson storage_manager user_manager audit_log lwip esp_timer
)
