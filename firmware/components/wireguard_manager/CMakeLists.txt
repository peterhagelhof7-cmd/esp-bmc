idf_component_register(
  SRCS "wireguard_manager.c"
  INCLUDE_DIRS "include"
  # esp_wireguard kommt als PlatformIO-"lib_deps"-Bibliothek (droscy/esp_wireguard,
  # kein idf_component.yml/ESP-IDF-Managed-Component - siehe platformio.ini).
  # PlatformIOs LDF verlinkt solche Bibliotheken erst am finalen ELF-Link-Schritt,
  # ausserhalb des CMake-Component-Graphen - daher ist esp_wireguard.h fuer
  # idf_component_register()-Komponenten unter components/ nicht automatisch
  # sichtbar. Pfad hier deshalb explizit ergaenzt.
  PRIV_INCLUDE_DIRS "../../.pio/libdeps/esp32-s3-devkitc-1-n16r8/esp_wireguard/include"
                     "../../.pio/libdeps/esp32-s3-devkitc-1-n16r8/esp_wireguard/src"
  REQUIRES lwip storage_manager espressif__cjson audit_log
)

