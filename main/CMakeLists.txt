idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        sc_core
        sc_config
        sc_health
        sc_status_led
        sc_wifi
        sc_relay
        sc_inputs
        sc_mqtt
        sc_webfs
        sc_web
        sc_time
        sc_timer
        sc_sensors
        sc_modbus
        sc_ha
        sc_diag
        sc_ota
        sc_automation
)
