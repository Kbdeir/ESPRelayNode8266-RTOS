idf_component_register(
    SRCS "sc_sensors.c"
    INCLUDE_DIRS "include"
    REQUIRES sc_config sc_mqtt sc_relay esp8266
)
