#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct{bool exists,enabled,active,paused;uint8_t id,relay,type;int32_t seconds_to_off;int32_t seconds_to_on;} sc_timer_status_t;
esp_err_t sc_timer_init(void);
void sc_timer_status(uint8_t id, sc_timer_status_t *out);
bool sc_timer_set_paused(uint8_t relay, bool paused);
