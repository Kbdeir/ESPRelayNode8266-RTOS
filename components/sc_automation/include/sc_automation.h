#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sc_config.h"

#define SC_REMOTE_STALE_SEC 300

typedef struct {
    bool enabled;
    bool source_available;
    bool gate_open;
    bool matched;
    uint32_t stable_sec;
    uint32_t seconds_since_fire;
} sc_automation_status_t;

typedef struct {
    bool configured;
    bool valid;
    float value;
    uint32_t age_sec;
} sc_remote_sensor_status_t;

esp_err_t sc_automation_init(void);
void sc_automation_tick(void);
void sc_automation_reset_rule(uint8_t index);
void sc_automation_reset_remote(uint8_t index);
void sc_automation_status(uint8_t index, sc_automation_status_t *out);
void sc_automation_remote_status(uint8_t index, sc_remote_sensor_status_t *out);
bool sc_automation_remote_message(const char *topic, size_t topic_len,
                                  const char *payload, size_t payload_len);
