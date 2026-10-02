#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct { uint32_t free_heap,min_free_heap,max_free_block,wifi_reconnects,mqtt_reconnects,http_requests,relay_commands; } sc_health_snapshot_t;
esp_err_t sc_health_init(void);
void sc_health_snapshot(sc_health_snapshot_t *out);
void sc_health_wifi_reconnect(void); void sc_health_mqtt_reconnect(void); void sc_health_http_request(void); void sc_health_relay_command(void);
