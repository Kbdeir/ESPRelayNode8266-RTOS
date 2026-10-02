#pragma once
#include "esp_err.h"
#include "sc_types.h"

#define SC_MAX_TIMERS 4
#define SC_MAX_INPUTS 3

typedef enum {
    SC_INPUT_NONE=0,
    SC_INPUT_MQTT_TOGGLE=1,
    SC_INPUT_MQTT_STATE=2,
    SC_INPUT_RELAY_TOGGLE=3,
    SC_INPUT_COPY_TO_RELAY=4,
    SC_INPUT_TEMPERATURE=5
} sc_input_mode_t;

typedef struct {
    uint8_t gpio;
    uint8_t mode;
    int8_t relay;
    char topic[SC_TOPIC_LEN];
} sc_input_config_t;

typedef struct {
    uint8_t enabled;
    uint8_t relay;
    float low_c;
    float high_c;
    float buffer_c;
} sc_temp_control_config_t;

typedef struct {
    uint8_t relay_nb;
    uint8_t gpio;
    uint8_t active_high;
    char pub_topic[SC_TOPIC_LEN];
    char state_pub_topic[SC_TOPIC_LEN];
    char sub_topic[SC_TOPIC_LEN];
    char lwill_topic[SC_TOPIC_LEN];
    char acs_topic[SC_TOPIC_LEN];
    uint32_t ttl_sec;
    uint32_t tta_sec;
    uint8_t acs_enabled;
    uint8_t acs_model_amps; /* 5,20,30 */
    uint16_t acs_zero;
    float max_current;
} sc_relay_config_t;

typedef enum {
    SC_TIMER_DISABLED=0,
    SC_TIMER_ONCE=1,
    SC_TIMER_DAILY=2,
    SC_TIMER_WEEKLY=3,
    SC_TIMER_MONTHLY=4
} sc_timer_type_t;

typedef struct {
    uint8_t enabled;
    uint8_t type;
    uint8_t relay;
    uint8_t weekdays; /* bit0 Monday ... bit6 Sunday */
    uint8_t month_day;
    uint8_t start_hour;
    uint8_t start_minute;
    uint32_t duration_sec;
    uint32_t once_epoch;
} sc_timer_config_t;

typedef struct {
    char date_from[11]; /* YYYY-MM-DD */
    char date_to[11];   /* YYYY-MM-DD */
    uint8_t end_hour;
    uint8_t end_minute;
    uint16_t duration_hours;
    uint8_t duration_minutes;
} sc_timer_schedule_t;

typedef struct {
    uint32_t version;
    char phy_loc[SC_LOCATION_LEN];
    char ssid[SC_SSID_LEN];
    char wifi_password[SC_PASS_LEN];
    char ap_ssid[SC_SSID_LEN];
    char ap_password[SC_PASS_LEN];
    char timeserver[SC_HOST_LEN];
    int8_t timezone_hours;

    uint8_t mqtt_active;
    char mqtt_broker[SC_HOST_LEN];
    uint16_t mqtt_port;
    uint16_t mqtt_keepalive_sec;
    char mqtt_user[SC_USER_LEN];
    char mqtt_password[SC_PASS_LEN];
    uint8_t ha_discovery_active;

    uint8_t modbus_active;
    uint16_t modbus_port;

    uint8_t ds18b20_active;
    uint8_t ds18b20_gpio;
    uint8_t ds18b20_second_gpio;
    char temp_topic1[SC_TOPIC_LEN];
    char temp_topic2[SC_TOPIC_LEN];

    uint8_t relay_count;
    sc_relay_config_t relay[SC_MAX_RELAYS];
    sc_timer_config_t timer[SC_MAX_TIMERS];
    /* Appended in config v3 so the complete v2 blob remains a stable prefix. */
    sc_timer_schedule_t timer_schedule[SC_MAX_TIMERS];
    /* Appended in config v4 so the complete v3 blob remains a stable prefix. */
    uint8_t input_count;
    sc_input_config_t input[SC_MAX_INPUTS];
    sc_temp_control_config_t temp_control;
    /* Appended in config v5 so the complete v4 blob remains a stable prefix. */
    uint8_t modbus_unit_id;
    uint8_t config_v5_padding[3];
    /* Appended in config v6 so the complete v5 blob remains a stable prefix. */
    char web_user[SC_USER_LEN];
    char web_password[SC_PASS_LEN];
} sc_config_t;

esp_err_t sc_config_init(void);
void sc_config_snapshot(sc_config_t *out);
const sc_config_t *sc_config_acquire(void);
void sc_config_release(void);
esp_err_t sc_config_replace(const sc_config_t *cfg);
void sc_config_defaults(sc_config_t *cfg);
