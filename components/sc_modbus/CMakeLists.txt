idf_component_register(
    SRCS "sc_modbus.c"
    INCLUDE_DIRS "include"
    REQUIRES sc_config sc_relay lwip
)
