idf_component_register(
    SRCS "sc_timer.c"
    INCLUDE_DIRS "include"
    REQUIRES sc_config sc_relay sc_time sc_sensors
)
