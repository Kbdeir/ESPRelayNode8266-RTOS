#pragma once

#include "esp_err.h"

typedef enum {
    SC_STATUS_LED_CONNECTING,
    SC_STATUS_LED_AP_MODE,
    SC_STATUS_LED_CONNECTED,
} sc_status_led_mode_t;

esp_err_t sc_status_led_init(void);
void sc_status_led_set_mode(sc_status_led_mode_t mode);
