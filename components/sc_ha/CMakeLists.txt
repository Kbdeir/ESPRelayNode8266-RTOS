idf_component_register(
    SRCS "sc_ha.c"
    INCLUDE_DIRS "include"
    REQUIRES sc_config sc_mqtt sc_sensors sc_relay esp8266
)
