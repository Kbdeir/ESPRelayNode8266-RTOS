#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct{bool has_dump;uint32_t reset_reason;uint32_t boot_count;char reason[32];} sc_diag_t;
esp_err_t sc_diag_init(void);
void sc_diag_snapshot(sc_diag_t*out);
esp_err_t sc_diag_clear(void);
