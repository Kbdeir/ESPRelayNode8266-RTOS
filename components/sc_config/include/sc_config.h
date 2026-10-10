#pragma once
#include "esp_err.h"
#include "sc_types.h"

#define SC_MAX_TIMERS 4
#define SC_MAX_INPUTS 3
#define SC_MAX_AUTOMATION_RULES 4
#define SC_MAX_REMOTE_SENSORS 8
#define SC_REMOTE_TOPIC_LEN 64
#define SC_REMOTE_KEY_LEN 32
#define SC_REMOTE_LABEL_LEN 32

typedef enum {
    SC_REMOTE_MQTT = 0,
    SC_REMOTE_MODBUS_TCP = 1
} sc_remote_transport_t;

typedef enum {
    SC_MODBUS_BOOL = 0,
    SC_MODBUS_U16 = 1,
    SC_MODBUS_I16 = 2,
    SC_MODBUS_U32 = 3,
    SC_MODBUS_I32 = 4,
    SC_MODBUS_F32 = 5
} sc_modbus_value_type_t;

typedef struct {
    uint16_t port;
    uint16_t address;
    uint16_t poll_sec;
    uint8_t unit;
    uint8_t function;
    uint8_t value_type;
    uint8_t word_order;
    float scale;
    float offset;
} sc_remote_modbus_config_t;

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

typedef enum {
    SC_AUTOMATION_SOURCE_NONE = 0,
    SC_AUTOMATION_SOURCE_TEMP1 = 1,
    SC_AUTOMATION_SOURCE_TEMP2 = 2,
    SC_AUTOMATION_SOURCE_ACS = 3,
    SC_AUTOMATION_SOURCE_INPUT1 = 4,
    SC_AUTOMATION_SOURCE_INPUT2 = 5,
    SC_AUTOMATION_SOURCE_INPUT3 = 6,
    SC_AUTOMATION_SOURCE_RELAY1 = 7,
    SC_AUTOMATION_SOURCE_RELAY2 = 8,
    SC_AUTOMATION_SOURCE_RELAY3 = 9,
    SC_AUTOMATION_SOURCE_RELAY4 = 10,
    SC_AUTOMATION_SOURCE_REMOTE1 = 20,
    SC_AUTOMATION_SOURCE_REMOTE2 = 21,
    SC_AUTOMATION_SOURCE_REMOTE3 = 22,
    SC_AUTOMATION_SOURCE_REMOTE4 = 23,
    SC_AUTOMATION_SOURCE_REMOTE5 = 24,
    SC_AUTOMATION_SOURCE_REMOTE6 = 25,
    SC_AUTOMATION_SOURCE_REMOTE7 = 26,
    SC_AUTOMATION_SOURCE_REMOTE8 = 27
} sc_automation_source_t;

typedef enum {
    SC_AUTOMATION_GREATER_THAN = 0,
    SC_AUTOMATION_LESS_THAN = 1,
    SC_AUTOMATION_BETWEEN = 2,
    SC_AUTOMATION_EQUAL = 3
} sc_automation_operator_t;

typedef enum {
    SC_AUTOMATION_ACTION_ON = 0,
    SC_AUTOMATION_ACTION_OFF = 1,
    SC_AUTOMATION_ACTION_TOGGLE = 2
} sc_automation_action_t;

typedef enum {
    SC_AUTOMATION_LOGIC_AND = 0,
    SC_AUTOMATION_LOGIC_OR = 1
} sc_automation_logic_t;

typedef enum {
    SC_AUTOMATION_ON_CHANGE = 0,
    SC_AUTOMATION_PERIODIC = 1
} sc_automation_trigger_t;

typedef struct {
    uint8_t source;
    uint8_t op;
    uint16_t reserved;
    float value1;
    float value2;
} sc_automation_condition_t;

typedef struct {
    uint8_t enabled;
    uint8_t target_relay;
    uint8_t action;
    uint8_t logic;
    uint8_t trigger_mode;
    uint8_t gate_timer;
    uint16_t hold_sec;
    uint16_t cooldown_sec;
    uint16_t reserved;
    sc_automation_condition_t condition[2];
} sc_automation_rule_t;

typedef struct {
    uint32_t ipv4;
    uint16_t port;
    uint16_t address;
    uint8_t unit;
    uint8_t remote;
    uint8_t send_clear;
    uint8_t reserved;
} sc_modbus_rule_target_t;

typedef struct {
    char topic[SC_REMOTE_TOPIC_LEN];
    char json_key[SC_REMOTE_KEY_LEN]; /* Binary Modbus metadata in Modbus mode. */
    char label[SC_REMOTE_LABEL_LEN];
} sc_remote_sensor_config_t;

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
    /* Appended in config v7 so the complete v6 blob remains a stable prefix. */
    sc_automation_rule_t automation[SC_MAX_AUTOMATION_RULES];
    sc_remote_sensor_config_t remote_sensor[SC_MAX_REMOTE_SENSORS];
    /* v8: keep the v7 prefix intact. Modbus metadata fits in json_key[]. */
    uint8_t remote_transport[SC_MAX_REMOTE_SENSORS];
    /* v9: outbound single-coil targets; earlier configuration stays a prefix. */
    sc_modbus_rule_target_t modbus_rule[SC_MAX_AUTOMATION_RULES];
} sc_config_t;

esp_err_t sc_config_init(void);
esp_err_t sc_config_storage_ready(void);
void sc_config_snapshot(sc_config_t *out);
const sc_config_t *sc_config_acquire(void);
void sc_config_release(void);
esp_err_t sc_config_replace(const sc_config_t *cfg);
esp_err_t sc_config_set_relay_timing(uint8_t relay, bool ttl,
                                     uint32_t seconds);
void sc_config_defaults(sc_config_t *cfg);
void sc_remote_modbus_decode(const sc_remote_sensor_config_t *sensor,
                             sc_remote_modbus_config_t *out);
void sc_remote_modbus_encode(sc_remote_sensor_config_t *sensor,
                             const sc_remote_modbus_config_t *value);
