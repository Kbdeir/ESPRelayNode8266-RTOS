#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sc_automation.h"
#include "sc_inputs.h"
#include "sc_relay.h"
#include "sc_sensors.h"
#include "sc_timer.h"

typedef struct {
    bool initialized;
    bool last_result;
    bool fired_for_state;
    bool source_available;
    bool gate_open;
    int64_t result_since_us;
    int64_t last_fired_us;
} rule_runtime_t;

typedef struct {
    float value;
    int64_t seen_us;
} remote_runtime_t;

static rule_runtime_t runtime[SC_MAX_AUTOMATION_RULES];
static remote_runtime_t remote[SC_MAX_REMOTE_SENSORS];
static const char *TAG = "automation";

static bool source_valid(uint8_t source)
{
    return source <= SC_AUTOMATION_SOURCE_RELAY4 ||
           (source >= SC_AUTOMATION_SOURCE_REMOTE1 &&
            source <= SC_AUTOMATION_SOURCE_REMOTE8);
}

static bool rule_valid(const sc_automation_rule_t *rule, uint8_t relay_count)
{
    if (!rule || rule->target_relay >= relay_count ||
        rule->action > SC_AUTOMATION_ACTION_TOGGLE ||
        rule->logic > SC_AUTOMATION_LOGIC_OR ||
        rule->trigger_mode > SC_AUTOMATION_PERIODIC ||
        rule->gate_timer > SC_MAX_TIMERS ||
        !source_valid(rule->condition[0].source) ||
        rule->condition[0].source == SC_AUTOMATION_SOURCE_NONE ||
        rule->condition[0].op > SC_AUTOMATION_EQUAL ||
        !source_valid(rule->condition[1].source) ||
        rule->condition[1].op > SC_AUTOMATION_EQUAL)
        return false;
    return !(rule->trigger_mode == SC_AUTOMATION_PERIODIC &&
             rule->action == SC_AUTOMATION_ACTION_TOGGLE);
}

static bool evaluate(const sc_automation_condition_t *condition, float value)
{
    switch ((sc_automation_operator_t)condition->op) {
    case SC_AUTOMATION_GREATER_THAN: return value > condition->value1;
    case SC_AUTOMATION_LESS_THAN: return value < condition->value1;
    case SC_AUTOMATION_BETWEEN:
        return value >= condition->value1 && value <= condition->value2;
    case SC_AUTOMATION_EQUAL: return fabsf(value - condition->value1) < 0.001f;
    default: return false;
    }
}

static bool read_remote(uint8_t index, bool configured, int64_t now_us,
                        float *value)
{
    if (index >= SC_MAX_REMOTE_SENSORS || !configured) return false;
    int64_t seen;
    float cached;
    taskENTER_CRITICAL();
    seen = remote[index].seen_us;
    cached = remote[index].value;
    taskEXIT_CRITICAL();
    if (!seen || now_us - seen > (int64_t)SC_REMOTE_STALE_SEC * 1000000LL)
        return false;
    *value = cached;
    return true;
}

static bool read_source(uint8_t source, const sc_sensor_snapshot_t *sensors,
                        const sc_input_status_t inputs[SC_MAX_INPUTS],
                        uint8_t relay_count,
                        const bool remote_configured[SC_MAX_REMOTE_SENSORS],
                        int64_t now_us, float *value)
{
    if (source == SC_AUTOMATION_SOURCE_TEMP1) {
        if (!sensors->temp1_valid) return false;
        *value = sensors->temp1_c;
        return true;
    }
    if (source == SC_AUTOMATION_SOURCE_TEMP2) {
        if (!sensors->temp2_valid) return false;
        *value = sensors->temp2_c;
        return true;
    }
    if (source == SC_AUTOMATION_SOURCE_ACS) {
        if (!sensors->acs_valid) return false;
        *value = sensors->acs_amps;
        return true;
    }
    if (source >= SC_AUTOMATION_SOURCE_INPUT1 &&
        source <= SC_AUTOMATION_SOURCE_INPUT3) {
        uint8_t index = source - SC_AUTOMATION_SOURCE_INPUT1;
        if (!inputs[index].configured) return false;
        *value = inputs[index].level ? 1.0f : 0.0f;
        return true;
    }
    if (source >= SC_AUTOMATION_SOURCE_RELAY1 &&
        source <= SC_AUTOMATION_SOURCE_RELAY4) {
        uint8_t index = source - SC_AUTOMATION_SOURCE_RELAY1;
        if (index >= relay_count) return false;
        *value = sc_relay_get(index) ? 1.0f : 0.0f;
        return true;
    }
    if (source >= SC_AUTOMATION_SOURCE_REMOTE1 &&
        source <= SC_AUTOMATION_SOURCE_REMOTE8) {
        uint8_t index = source - SC_AUTOMATION_SOURCE_REMOTE1;
        return read_remote(index, remote_configured[index], now_us, value);
    }
    return false;
}

static bool apply_action(const sc_automation_rule_t *rule, bool result)
{
    sc_relay_cmd_t command = {.relay = rule->target_relay};
    if (rule->action == SC_AUTOMATION_ACTION_TOGGLE) {
        command.op = SC_RELAY_TOGGLE;
    } else {
        bool desired = rule->action == SC_AUTOMATION_ACTION_ON ? result : !result;
        if (sc_relay_get(rule->target_relay) == desired) return true;
        command.op = SC_RELAY_SET;
        command.value = desired;
    }
    return sc_relay_submit(&command);
}

esp_err_t sc_automation_init(void)
{
    memset(runtime, 0, sizeof(runtime));
    memset(remote, 0, sizeof(remote));
    return ESP_OK;
}

void sc_automation_reset_rule(uint8_t index)
{
    if (index >= SC_MAX_AUTOMATION_RULES) return;
    taskENTER_CRITICAL();
    int64_t last_fired_us = runtime[index].last_fired_us;
    memset(&runtime[index], 0, sizeof(runtime[index]));
    runtime[index].last_fired_us = last_fired_us;
    taskEXIT_CRITICAL();
}

void sc_automation_reset_remote(uint8_t index)
{
    if (index >= SC_MAX_REMOTE_SENSORS) return;
    taskENTER_CRITICAL();
    memset(&remote[index], 0, sizeof(remote[index]));
    taskEXIT_CRITICAL();
}

void sc_automation_tick(void)
{
    sc_sensor_snapshot_t sensors;
    sc_input_status_t inputs[SC_MAX_INPUTS];
    sc_automation_rule_t rules[SC_MAX_AUTOMATION_RULES];
    bool remote_configured[SC_MAX_REMOTE_SENSORS];
    uint8_t relay_count;
    sc_sensors_snapshot(&sensors);
    sc_inputs_snapshot(inputs);

    const sc_config_t *config = sc_config_acquire();
    relay_count = config->relay_count <= SC_MAX_RELAYS ? config->relay_count : SC_MAX_RELAYS;
    memcpy(rules, config->automation, sizeof(rules));
    for (uint8_t i = 0; i < SC_MAX_REMOTE_SENSORS; i++)
        remote_configured[i] = config->remote_sensor[i].topic[0] != 0;
    sc_config_release();

    int64_t now_us = esp_timer_get_time();
    if (!sensors.updated_us || now_us - sensors.updated_us > 10000000LL) {
        sensors.temp1_valid = false;
        sensors.temp2_valid = false;
        sensors.acs_valid = false;
    }
    for (uint8_t i = 0; i < SC_MAX_AUTOMATION_RULES; i++) {
        sc_automation_rule_t *rule = &rules[i];
        if (!rule->enabled || !rule_valid(rule, relay_count)) {
            sc_automation_reset_rule(i);
            continue;
        }

        bool gate_open = true;
        if (rule->gate_timer) {
            sc_timer_status_t timer;
            sc_timer_status(rule->gate_timer, &timer);
            gate_open = timer.enabled && timer.active;
        }

        float value0 = 0.0f, value1 = 0.0f;
        bool available0 = read_source(rule->condition[0].source, &sensors, inputs,
                                      relay_count, remote_configured, now_us,
                                      &value0);
        bool has_second = rule->condition[1].source != SC_AUTOMATION_SOURCE_NONE;
        bool available1 = !has_second ||
            read_source(rule->condition[1].source, &sensors, inputs, relay_count,
                        remote_configured, now_us, &value1);
        if (!available0 || !available1) {
            taskENTER_CRITICAL();
            runtime[i].initialized = false;
            runtime[i].source_available = false;
            runtime[i].gate_open = gate_open;
            taskEXIT_CRITICAL();
            continue;
        }

        bool result = gate_open && evaluate(&rule->condition[0], value0);
        if (gate_open && has_second) {
            bool second = evaluate(&rule->condition[1], value1);
            result = rule->logic == SC_AUTOMATION_LOGIC_AND
                         ? result && second
                         : result || second;
        }

        rule_runtime_t current;
        taskENTER_CRITICAL();
        current = runtime[i];
        taskEXIT_CRITICAL();
        current.source_available = true;
        current.gate_open = gate_open;
        if (!current.initialized || current.last_result != result) {
            current.initialized = true;
            current.last_result = result;
            current.fired_for_state = false;
            current.result_since_us = now_us;
        }

        bool ready = now_us - current.result_since_us >=
                         (int64_t)rule->hold_sec * 1000000LL &&
                     (!current.last_fired_us ||
                      now_us - current.last_fired_us >=
                          (int64_t)rule->cooldown_sec * 1000000LL);
        bool should_fire = ready &&
            (rule->trigger_mode == SC_AUTOMATION_PERIODIC ||
             !current.fired_for_state);
        if (should_fire && apply_action(rule, result)) {
            current.last_fired_us = now_us;
            if (rule->trigger_mode == SC_AUTOMATION_ON_CHANGE)
                current.fired_for_state = true;
            ESP_LOGI(TAG, "rule %u %s, relay %u action %u",
                     (unsigned)(i + 1), result ? "matched" : "cleared",
                     (unsigned)rule->target_relay, (unsigned)rule->action);
        }
        taskENTER_CRITICAL();
        runtime[i] = current;
        taskEXIT_CRITICAL();
    }
}

void sc_automation_status(uint8_t index, sc_automation_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (index >= SC_MAX_AUTOMATION_RULES) return;
    rule_runtime_t current;
    taskENTER_CRITICAL();
    current = runtime[index];
    taskEXIT_CRITICAL();
    const sc_config_t *config = sc_config_acquire();
    out->enabled = config->automation[index].enabled;
    sc_config_release();
    out->source_available = current.source_available;
    out->gate_open = current.gate_open;
    out->matched = current.initialized && current.last_result;
    int64_t now = esp_timer_get_time();
    if (current.initialized && now > current.result_since_us)
        out->stable_sec = (uint32_t)((now - current.result_since_us) / 1000000LL);
    if (current.last_fired_us && now > current.last_fired_us)
        out->seconds_since_fire = (uint32_t)((now - current.last_fired_us) / 1000000LL);
}

void sc_automation_remote_status(uint8_t index, sc_remote_sensor_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (index >= SC_MAX_REMOTE_SENSORS) return;
    const sc_config_t *config = sc_config_acquire();
    out->configured = config->remote_sensor[index].topic[0] != 0;
    sc_config_release();
    int64_t seen;
    taskENTER_CRITICAL();
    seen = remote[index].seen_us;
    out->value = remote[index].value;
    taskEXIT_CRITICAL();
    if (seen) {
        int64_t age = esp_timer_get_time() - seen;
        out->age_sec = age > 0 ? (uint32_t)(age / 1000000LL) : 0;
        out->valid = out->configured && out->age_sec <= SC_REMOTE_STALE_SEC;
    }
}

static bool parse_scalar(const char *text, size_t len, float *out)
{
    while (len && isspace((unsigned char)*text)) { text++; len--; }
    while (len && isspace((unsigned char)text[len - 1])) len--;
    bool quoted = len >= 2 && text[0] == '"';
    if (quoted) {
        text++;
        len--;
        const char *end = memchr(text, '"', len);
        if (!end) return false;
        len = (size_t)(end - text);
    } else {
        size_t n = 0;
        while (n < len && text[n] != ',' && text[n] != '}' && text[n] != ']') n++;
        len = n;
        while (len && isspace((unsigned char)text[len - 1])) len--;
    }
    if ((len == 2 && !strncasecmp(text, "on", 2)) ||
        (len == 4 && !strncasecmp(text, "true", 4)) ||
        (len == 3 && !strncasecmp(text, "yes", 3))) {
        *out = 1.0f;
        return true;
    }
    if ((len == 3 && !strncasecmp(text, "off", 3)) ||
        (len == 5 && !strncasecmp(text, "false", 5)) ||
        (len == 2 && !strncasecmp(text, "no", 2))) {
        *out = 0.0f;
        return true;
    }
    if (!len || len >= 40) return false;
    char number[40];
    memcpy(number, text, len);
    number[len] = 0;
    char *end = NULL;
    float value = strtof(number, &end);
    if (!end || end == number || *end || !isfinite(value)) return false;
    *out = value;
    return true;
}

static bool parse_json_key(const char *payload, size_t len, const char *key,
                           float *out)
{
    size_t key_len = strlen(key);
    if (!key_len) return parse_scalar(payload, len, out);
    for (size_t i = 0; i + key_len + 2 < len; i++) {
        if (payload[i] != '"' || memcmp(payload + i + 1, key, key_len) ||
            payload[i + key_len + 1] != '"') continue;
        size_t p = i + key_len + 2;
        while (p < len && isspace((unsigned char)payload[p])) p++;
        if (p >= len || payload[p++] != ':') continue;
        while (p < len && isspace((unsigned char)payload[p])) p++;
        return p < len && parse_scalar(payload + p, len - p, out);
    }
    return false;
}

bool sc_automation_remote_message(const char *topic, size_t topic_len,
                                  const char *payload, size_t payload_len)
{
    if (!topic || !topic_len || !payload || !payload_len) return false;
    int matched = -1;
    char key[SC_REMOTE_KEY_LEN] = {0};
    const sc_config_t *config = sc_config_acquire();
    for (uint8_t i = 0; i < SC_MAX_REMOTE_SENSORS; i++) {
        const char *configured = config->remote_sensor[i].topic;
        size_t configured_len = strlen(configured);
        if (configured_len == topic_len &&
            !memcmp(configured, topic, topic_len)) {
            matched = i;
            snprintf(key, sizeof(key), "%s", config->remote_sensor[i].json_key);
            break;
        }
    }
    sc_config_release();
    if (matched < 0) return false;

    const char *first = payload;
    size_t remaining = payload_len;
    while (remaining && isspace((unsigned char)*first)) { first++; remaining--; }
    float value;
    bool json = remaining && (*first == '{' || *first == '[');
    bool ok = json && key[0] ? parse_json_key(first, remaining, key, &value)
                             : parse_scalar(first, remaining, &value);
    if (!ok) {
        ESP_LOGW(TAG, "remote sensor %d payload could not be parsed", matched + 1);
        return true;
    }
    taskENTER_CRITICAL();
    remote[matched].value = value;
    remote[matched].seen_us = esp_timer_get_time();
    taskEXIT_CRITICAL();
    return true;
}
