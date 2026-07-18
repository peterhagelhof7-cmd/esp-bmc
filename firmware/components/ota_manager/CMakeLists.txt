idf_component_register(
  SRCS "ota_manager.c"
  INCLUDE_DIRS "include"
  REQUIRES app_update firmware_version
)
