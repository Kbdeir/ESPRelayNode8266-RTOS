#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum {SC_RELAY_SET,SC_RELAY_TOGGLE} sc_relay_op_t;
typedef struct {uint8_t relay; sc_relay_op_t op; bool value;} sc_relay_cmd_t;
typedef struct {bool state;bool ttl_active;bool tta_active;uint32_t ttl_remaining_sec;uint32_t tta_remaining_sec;} sc_relay_status_t;
esp_err_t sc_relay_init(void); bool sc_relay_submit(const sc_relay_cmd_t *cmd); bool sc_relay_get(uint8_t relay); bool sc_relay_status(uint8_t relay,sc_relay_status_t *out); uint8_t sc_relay_count(void);
bool sc_relay_set_timing(uint8_t relay, bool ttl, uint32_t seconds);
