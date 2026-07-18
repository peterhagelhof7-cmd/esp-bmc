idf_component_register(
  SRCS "watchdog_manager.c"
  INCLUDE_DIRS "include"
  REQUIRES led_strip esp_system
)
