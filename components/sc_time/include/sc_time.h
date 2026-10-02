#pragma once
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
esp_err_t sc_time_init(void);
bool sc_time_valid(void);
time_t sc_time_now_local(void);
void sc_time_format(char *out, unsigned cap);
