idf_component_register(SRCS "usb_port.c")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include")
