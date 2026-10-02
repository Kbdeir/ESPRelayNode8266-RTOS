idf_component_register(
    SRCS "sc_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES sc_config sc_health sc_relay mqtt
)
