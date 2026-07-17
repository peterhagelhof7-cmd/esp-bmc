idf_component_register(
  SRCS "sensor_manager.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_driver_gpio esp_adc esp_timer config_manager notification_manager sensor_history
)
