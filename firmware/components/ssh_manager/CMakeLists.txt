idf_component_register(
  SRCS "ssh_manager.c"
  INCLUDE_DIRS "include"
  REQUIRES wolfssl__wolfssh storage_manager audit_log usb_manager user_manager lwip
)
