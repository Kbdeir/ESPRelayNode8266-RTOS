#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sc_config.h"
typedef struct{bool temp1_valid,temp2_valid,acs_valid,temp_control_enabled,temp_control_call;float temp1_c,temp2_c,acs_amps,temp_delta_c;int64_t updated_us;} sc_sensor_snapshot_t;
esp_err_t sc_sensors_init(void);
esp_err_t sc_sensors_ensure_started(void);
void sc_sensors_poll(void);
bool sc_sensors_validate_config(const sc_config_t *cfg);
void sc_sensors_snapshot(sc_sensor_snapshot_t*out);
