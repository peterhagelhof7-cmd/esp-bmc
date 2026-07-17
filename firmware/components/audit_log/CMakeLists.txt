idf_component_register(
  SRCS "audit_log.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_timer storage_manager time_manager
)
