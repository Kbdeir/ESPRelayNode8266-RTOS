#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sc_config.h"
#include "sc_mqtt.h"
#include "sc_ha.h"
static volatile int requested = 1;
static const char *TAG = "ha";
void sc_ha_request_publish(void) { requested = 1; }
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
    static char node[48], b[768], phy[SC_LOCATION_LEN], sub[SC_TOPIC_LEN], state[SC_TOPIC_LEN], acs[SC_TOPIC_LEN], temp1[SC_TOPIC_LEN], temp2[SC_TOPIC_LEN];
    ident(node, sizeof(node));
    sc_mqtt_status_t mqtt;
    sc_mqtt_status(&mqtt);
    const sc_config_t *c = sc_config_acquire();
    int count = c->relay_count <= SC_MAX_RELAYS ? c->relay_count : SC_MAX_RELAYS;
    bool ds = c->ds18b20_active;
    snprintf(phy, sizeof(phy), "%s", c->phy_loc);
    snprintf(temp1, sizeof(temp1), "%s", c->temp_topic1);
    snprintf(temp2, sizeof(temp2), "%s", c->temp_topic2);
    sc_config_release();
    for (int i = 0; i < count; i++)
    {
        c = sc_config_acquire();
        snprintf(sub, sizeof(sub), "%s", c->relay[i].sub_topic);
        snprintf(state, sizeof(state), "%s", c->relay[i].state_pub_topic);
        snprintf(acs, sizeof(acs), "%s", c->relay[i].acs_topic);
        bool acs_enabled = c->relay[i].acs_enabled;
        sc_config_release();
        snprintf(b, sizeof(b), "{\"name\":\"Relay %d\",\"uniq_id\":\"%s_relay%d\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"pl_on\":\"on\",\"pl_off\":\"off\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"SmartConfig\",\"mdl\":\"ESP8266\"}}", i, node, i, sub, state, mqtt.health_topic, node, phy);
        char id[24];
        snprintf(id, sizeof(id), "relay%d", i);
        ok = pub("switch", id, b) && ok;
        vTaskDelay(pdMS_TO_TICKS(300));
        if (acs_enabled)
        {
            snprintf(b, sizeof(b), "{\"name\":\"Relay %d Current\",\"uniq_id\":\"%s_current%d\",\"stat_t\":\"%s\",\"dev_cla\":\"current\",\"unit_of_meas\":\"A\",\"stat_cla\":\"measurement\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"%s\"]}}", i, node, i, acs, mqtt.health_topic, node);
            snprintf(id, sizeof(id), "current%d", i);
            ok = pub("sensor", id, b) && ok;
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
    if (ds)
    {
        snprintf(b, sizeof(b), "{\"name\":\"Tank Temperature\",\"uniq_id\":\"%s_temp1\",\"stat_t\":\"%s\",\"dev_cla\":\"temperature\",\"unit_of_meas\":\"°C\",\"stat_cla\":\"measurement\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"%s\"]}}", node, temp1, mqtt.health_topic, node);
        ok = pub("sensor", "temperature1", b) && ok;
        vTaskDelay(pdMS_TO_TICKS(300));
        snprintf(b, sizeof(b), "{\"name\":\"Panel Temperature\",\"uniq_id\":\"%s_temp2\",\"stat_t\":\"%s\",\"dev_cla\":\"temperature\",\"unit_of_meas\":\"°C\",\"stat_cla\":\"measurement\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"%s\"]}}", node, temp2, mqtt.health_topic, node);
        ok = pub("sensor", "temperature2", b) && ok;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    return ok;
}
static void task(void *p)
{
    (void)p;
    bool prev = false;
    for (;;)
    {
        sc_mqtt_status_t m;
        sc_mqtt_status(&m);
        const sc_config_t *c = sc_config_acquire();
        bool discovery = c->ha_discovery_active;
        sc_config_release();
        if (m.connected && !prev)
            requested = 1;
        if (discovery && m.connected && requested)
        {
            if (publish_all())
                requested = 0;
            else
                ESP_LOGW(TAG, "discovery publish incomplete; retrying");
        }
        prev = m.connected;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
esp_err_t sc_ha_init(void) { return xTaskCreate(task, "ha_disc", 2560, NULL, 2, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM; }
