#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sc_config.h"
esp_err_t sc_modbus_init(void);
esp_err_t sc_modbus_ensure_started(void);
void sc_modbus_remote_changed(uint8_t index);
bool sc_modbus_rule_submit(uint8_t index,
    const sc_modbus_rule_target_t *target, bool on);
