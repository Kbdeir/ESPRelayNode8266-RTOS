#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sc_config.h"
#include "sc_mqtt.h"
#include "sc_ha.h"
static bool worker_active;
static const char *TAG = "ha";
static void ident(char *out, size_t n)
{
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(out, n, "smartconfig_%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}
static bool pub(const char *comp, const char *id, const char *payload)
{
    char topic[160];
    char node[48];
    ident(node, sizeof(node));
    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config", comp, node, id);
    bool ok = sc_mqtt_publish(topic, payload, true);
    if (ok)
        ESP_LOGI(TAG, "queued discovery: %s (%u bytes)", topic, (unsigned)strlen(payload));
    else
        ESP_LOGW(TAG, "failed to queue discovery: %s", topic);
    return ok;
}
static bool publish_all(void)
{
    bool ok = true;
    static char node[48], b[672], phy[SC_LOCATION_LEN], sub[SC_TOPIC_LEN], state[SC_TOPIC_LEN], acs[SC_TOPIC_LEN], temp1[SC_TOPIC_LEN], temp2[SC_TOPIC_LEN];
    ident(node, sizeof(node));
    sc_mqtt_status_t mqtt;
    sc_mqtt_status(&mqtt);
    const sc_config_t *c = sc_config_acquire();
    int count = c->relay_count <= SC_MAX_RELAYS ? c->relay_count : SC_MAX_RELAYS;
    bool ds = c->ds18b20_active;
    snprintf(phy, sizeof(phy), "%s", c->phy_loc);
    snprintf(temp1, sizeof(temp1), "%s", c->temp_topic1);
    snprintf(temp2, sizeof(temp2), "%s", c->temp_topic2);
    int input_count = c->input_count <= SC_MAX_INPUTS ? c->input_count : SC_MAX_INPUTS;
    sc_config_release();
    for (int i = 0; i < count; i++)
    {
        char pub_topic[SC_TOPIC_LEN];
        char timing_command[SC_TOPIC_LEN];
        c = sc_config_acquire();
        snprintf(sub, sizeof(sub), "%s", c->relay[i].sub_topic);
        snprintf(state, sizeof(state), "%s", c->relay[i].state_pub_topic);
        snprintf(pub_topic, sizeof(pub_topic), "%s", c->relay[i].pub_topic);
        snprintf(acs, sizeof(acs), "%s", c->relay[i].acs_topic);
        uint32_t ttl_sec = c->relay[i].ttl_sec;
        uint32_t tta_sec = c->relay[i].tta_sec;
        bool acs_enabled = c->relay[i].acs_enabled;
        sc_config_release();
        snprintf(b, sizeof(b), "{\"name\":\"Relay %d\",\"uniq_id\":\"%s_relay%d\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"pl_on\":\"on\",\"pl_off\":\"off\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"SmartConfig\",\"mdl\":\"ESP8266\"}}", i, node, i, sub, state, mqtt.health_topic, node, phy);
        char id[24];
        snprintf(id, sizeof(id), "relay%d", i);
        ok = pub("switch", id, b) && ok;
        vTaskDelay(pdMS_TO_TICKS(300));

        int ttl_len = snprintf(state, sizeof(state), "%s/TTL", pub_topic);
        int ttl_command_len = snprintf(timing_command, sizeof(timing_command),
                                       "%s/TTL/set", pub_topic);
        if (ttl_len > 0 && ttl_len < (int)sizeof(state) &&
            ttl_command_len > 0 &&
            ttl_command_len < (int)sizeof(timing_command))
        {
            char value[12];
            snprintf(value, sizeof(value), "%u", (unsigned)ttl_sec);
            ok = sc_mqtt_publish_qos0(state, value, true) && ok;
            snprintf(id, sizeof(id), "ttl%d", i);
            ok = pub("sensor", id, "") && ok;
            vTaskDelay(pdMS_TO_TICKS(100));
            snprintf(b, sizeof(b), "{\"name\":\"Relay %d TTL\",\"uniq_id\":\"%s_ttl%d\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\",\"dev_cla\":\"duration\",\"unit_of_meas\":\"s\",\"min\":0,\"max\":4294967295,\"step\":1,\"mode\":\"box\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"%s\"]}}", i, node, i, state, timing_command, mqtt.health_topic, node);
            ok = pub("number", id, b) && ok;
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        else
        {
            ESP_LOGW(TAG, "relay %d TTL topic is too long", i);
            ok = false;
        }

        int tta_len = snprintf(state, sizeof(state), "%s/TTA", pub_topic);
        int tta_command_len = snprintf(timing_command, sizeof(timing_command),
                                       "%s/TTA/set", pub_topic);
        if (tta_len > 0 && tta_len < (int)sizeof(state) &&
            tta_command_len > 0 &&
            tta_command_len < (int)sizeof(timing_command))
        {
            char value[12];
            snprintf(value, sizeof(value), "%u", (unsigned)tta_sec);
            ok = sc_mqtt_publish_qos0(state, value, true) && ok;
            snprintf(id, sizeof(id), "tta%d", i);
            ok = pub("sensor", id, "") && ok;
            vTaskDelay(pdMS_TO_TICKS(100));
            snprintf(b, sizeof(b), "{\"name\":\"Relay %d TTA\",\"uniq_id\":\"%s_tta%d\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\",\"dev_cla\":\"duration\",\"unit_of_meas\":\"s\",\"min\":0,\"max\":4294967295,\"step\":1,\"mode\":\"box\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"%s\"]}}", i, node, i, state, timing_command, mqtt.health_topic, node);
            ok = pub("number", id, b) && ok;
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        else
        {
            ESP_LOGW(TAG, "relay %d TTA topic is too long", i);
            ok = false;
        }

        if (acs_enabled)
        {
            snprintf(b, sizeof(b), "{\"name\":\"Relay %d Current\",\"uniq_id\":\"%s_current%d\",\"stat_t\":\"%s\",\"dev_cla\":\"current\",\"unit_of_meas\":\"A\",\"stat_cla\":\"measurement\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"%s\"]}}", i, node, i, acs, mqtt.health_topic, node);
            snprintf(id, sizeof(id), "current%d", i);
            ok = pub("sensor", id, b) && ok;
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
    for (int i = 0; i < input_count; i++)
    {
        char input_topic[SC_TOPIC_LEN];
        c = sc_config_acquire();
        uint8_t input_mode = c->input[i].mode;
        snprintf(input_topic, sizeof(input_topic), "%s", c->input[i].topic);
        sc_config_release();
        if (input_mode == SC_INPUT_TEMPERATURE || !input_topic[0])
            continue;
        int state_len = snprintf(state, sizeof(state), "%s/State", input_topic);
        if (state_len <= 0 || state_len >= (int)sizeof(state))
        {
            ESP_LOGW(TAG, "input %d state topic is too long", i + 1);
            continue;
        }
        snprintf(b, sizeof(b), "{\"name\":\"Input %d\",\"uniq_id\":\"%s_input%d\",\"stat_t\":\"%s\",\"pl_on\":\"on\",\"pl_off\":\"off\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"%s\"]}}", i + 1, node, i + 1, state, mqtt.health_topic, node);
        char id[24];
        snprintf(id, sizeof(id), "input%d", i + 1);
        ok = pub("binary_sensor", id, b) && ok;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (ds)
    {
        snprintf(b, sizeof(b), "{\"name\":\"Temperature 1\",\"uniq_id\":\"%s_temp1\",\"stat_t\":\"%s\",\"dev_cla\":\"temperature\",\"unit_of_meas\":\"°C\",\"stat_cla\":\"measurement\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"%s\"]}}", node, temp1, mqtt.health_topic, node);
        ok = pub("sensor", "temperature1", b) && ok;
        vTaskDelay(pdMS_TO_TICKS(300));
        snprintf(b, sizeof(b), "{\"name\":\"Temperature 2\",\"uniq_id\":\"%s_temp2\",\"stat_t\":\"%s\",\"dev_cla\":\"temperature\",\"unit_of_meas\":\"°C\",\"stat_cla\":\"measurement\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"%s\"]}}", node, temp2, mqtt.health_topic, node);
        ok = pub("sensor", "temperature2", b) && ok;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    return ok;
}
static void task(void *p)
{
    (void)p;
    for (;;)
    {
        sc_mqtt_status_t m;
        sc_mqtt_status(&m);
        const sc_config_t *c = sc_config_acquire();
        bool discovery = c->ha_discovery_active;
        sc_config_release();
        if (!discovery || !m.connected) break;
        if (publish_all()) break;
        ESP_LOGW(TAG, "discovery publish incomplete; retrying");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    taskENTER_CRITICAL();
    worker_active = false;
    taskEXIT_CRITICAL();
    vTaskDelete(NULL);
}

void sc_ha_request_publish(void)
{
    sc_mqtt_status_t mqtt;
    sc_mqtt_status(&mqtt);
    const sc_config_t *config = sc_config_acquire();
    bool discovery = config->ha_discovery_active;
    sc_config_release();
    if (!discovery || !mqtt.connected) return;
    taskENTER_CRITICAL();
    bool start = !worker_active;
    if (start) worker_active = true;
    taskEXIT_CRITICAL();
    if (!start) return;
    if (xTaskCreate(task, "ha_disc", 2560, NULL, 2, NULL) != pdPASS) {
        taskENTER_CRITICAL();
        worker_active = false;
        taskEXIT_CRITICAL();
        ESP_LOGE(TAG, "could not start discovery worker");
    }
}

void sc_ha_mqtt_connected(void)
{
    sc_ha_request_publish();
}

esp_err_t sc_ha_init(void)
{
    sc_ha_request_publish();
    return ESP_OK;
}
