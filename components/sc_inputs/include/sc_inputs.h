#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sc_config.h"

typedef struct {
    bool configured;
    bool level;
    uint8_t gpio;
    uint8_t mode;
    int8_t relay;
} sc_input_status_t;

esp_err_t sc_inputs_init(void);
void sc_inputs_poll(void);
void sc_inputs_snapshot(sc_input_status_t out[SC_MAX_INPUTS]);
