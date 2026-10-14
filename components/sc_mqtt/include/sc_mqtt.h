#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct {bool enabled,connected; char server[64],health_topic[96]; uint16_t port;} sc_mqtt_status_t;
/* QoS 1 enqueue without waiting/logging on a busy pool, for paced discovery. */
bool sc_mqtt_try_publish(const char *topic, const char *payload, bool retain);
/* Minimum free publisher stack in bytes; zero when the task is not running. */
uint32_t sc_mqtt_publisher_stack_free(void);
esp_err_t sc_mqtt_init(void); void sc_mqtt_status(sc_mqtt_status_t*out); bool sc_mqtt_connected(void); bool sc_mqtt_publish(const char*topic,const char*payload,bool retain); bool sc_mqtt_publish_qos0(const char*topic,const char*payload,bool retain); void sc_mqtt_update_remote_subscription(const char*old_topic,const char*new_topic); void sc_mqtt_relay_config_saved(uint8_t relay,const char*old_sub_topic,const char*old_pub_topic);
