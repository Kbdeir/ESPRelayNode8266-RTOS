#pragma once
#include <stddef.h>
#include "esp_err.h"
esp_err_t sc_ota_begin(size_t size);
esp_err_t sc_ota_write(const void*data,size_t len);
esp_err_t sc_ota_finish(void);
void sc_ota_abort(void);
