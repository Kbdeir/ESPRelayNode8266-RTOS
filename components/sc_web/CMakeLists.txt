idf_component_register(
    SRCS "sc_web.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json sc_config sc_health sc_wifi sc_relay sc_inputs sc_mqtt sc_webfs sc_sensors sc_timer sc_time sc_diag sc_ota sc_ha sc_automation sc_modbus lwip
)
