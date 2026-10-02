#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef struct {bool enabled,connected; char server[64],health_topic[96]; uint16_t port;} sc_mqtt_status_t;
esp_err_t sc_mqtt_init(void); void sc_mqtt_status(sc_mqtt_status_t*out); bool sc_mqtt_publish(const char*topic,const char*payload,bool retain); bool sc_mqtt_publish_qos0(const char*topic,const char*payload,bool retain);
