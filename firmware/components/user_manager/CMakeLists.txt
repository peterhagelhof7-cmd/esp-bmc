idf_component_register(
  SRCS "user_manager.c"
  INCLUDE_DIRS "include"
  REQUIRES espressif__cjson storage_manager mbedtls esp_hw_support esp_timer
)
