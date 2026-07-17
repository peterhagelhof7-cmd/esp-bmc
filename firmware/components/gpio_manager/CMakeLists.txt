idf_component_register(
  SRCS "gpio_manager.c"
  INCLUDE_DIRS "include"
  # ESP-IDF 5.x+ hat den frueheren monolithischen "driver"-Component in
  # Einzelkomponenten aufgeteilt - fuer driver/gpio.h wird esp_driver_gpio
  # gebraucht, nicht mehr nur "driver".
  REQUIRES esp_driver_gpio config_manager audit_log
)
