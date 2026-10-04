idf_component_register(
    SRCS "sc_automation.c"
    INCLUDE_DIRS "include"
    REQUIRES sc_config sc_sensors sc_inputs sc_relay sc_timer
)
