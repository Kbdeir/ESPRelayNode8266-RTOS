idf_component_register(
    SRCS "sc_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES sc_config sc_health sc_status_led esp8266
)
