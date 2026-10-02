#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sc_config.h"
#include "sc_health.h"
#include "sc_relay.h"
#include "sc_mqtt.h"

#define PUB_SLOTS 6
#define PUB_PAYLOAD_LEN 768

typedef struct {
    char topic[SC_TOPIC_LEN];
    uint8_t slot;
    uint8_t retain;
    uint8_t qos;
} pub_t;

typedef struct {
    char pub_topic[SC_TOPIC_LEN];
    char state_pub_topic[SC_TOPIC_LEN];
    char sub_topic[SC_TOPIC_LEN];
} mqtt_relay_config_t;

typedef struct {
    uint8_t mqtt_active;
    char mqtt_broker[SC_HOST_LEN];
    uint16_t mqtt_port;
    uint16_t mqtt_keepalive_sec;
    char mqtt_user[SC_USER_LEN];
    char mqtt_password[SC_PASS_LEN];
    uint8_t relay_count;
    mqtt_relay_config_t relay[SC_MAX_RELAYS];
} mqtt_runtime_config_t;

static esp_mqtt_client_handle_t client;
static QueueHandle_t pq;
static SemaphoreHandle_t pool_lock;
static mqtt_runtime_config_t cfg;
static volatile bool connected;
static char client_id[40];
static char will_topic[SC_TOPIC_LEN];
static char payload_pool[PUB_SLOTS][PUB_PAYLOAD_LEN];
static bool payload_used[PUB_SLOTS];
static const char *TAG = "mqtt";

static int topic_eq(esp_mqtt_event_handle_t e, const char *topic)
{
    size_t n = strlen(topic);
    return e->topic_len == (int)n && !strncmp(e->topic, topic, n);
}

static int payload_acquire(const char *payload)
{
    size_t n = strlen(payload) + 1;
    if (n > PUB_PAYLOAD_LEN) {
        ESP_LOGW(TAG, "payload too large: %u bytes", (unsigned)n);
        return -1;
    }
    if (!pool_lock || xSemaphoreTake(pool_lock, pdMS_TO_TICKS(50)) != pdTRUE) return -1;
    int slot = -1;
    for (int i = 0; i < PUB_SLOTS; i++) {
        if (!payload_used[i]) {
            payload_used[i] = true;
            memcpy(payload_pool[i], payload, n);
            slot = i;
            break;
        }
    }
    xSemaphoreGive(pool_lock);
    return slot;
}

static void payload_release(uint8_t slot)
{
    if (slot >= PUB_SLOTS || !pool_lock) return;
    if (xSemaphoreTake(pool_lock, portMAX_DELAY) == pdTRUE) {
        payload_used[slot] = false;
        xSemaphoreGive(pool_lock);
    }
}

static esp_err_t event_cb(esp_mqtt_event_handle_t e)
{
    switch (e->event_id) {
    case MQTT_EVENT_CONNECTED:
        connected = true;
        for (int i = 0; i < cfg.relay_count; i++) {
            esp_mqtt_client_subscribe(client, cfg.relay[i].sub_topic, 1);
            sc_mqtt_publish(cfg.relay[i].state_pub_topic, sc_relay_get(i) ? "on" : "off", true);
        }
        sc_mqtt_publish(will_topic, "online", true);
        break;
    case MQTT_EVENT_DISCONNECTED:
        connected = false;
        sc_health_mqtt_reconnect();
        break;
    case MQTT_EVENT_DATA:
        for (int i = 0; i < cfg.relay_count; i++) {
            if (!topic_eq(e, cfg.relay[i].sub_topic)) continue;
            sc_relay_cmd_t cmd = {.relay = i};
            if (e->data_len == 2 && !strncasecmp(e->data, "on", 2)) {
                cmd.op = SC_RELAY_SET;
                cmd.value = true;
            } else if (e->data_len == 3 && !strncasecmp(e->data, "off", 3)) {
                cmd.op = SC_RELAY_SET;
                cmd.value = false;
            } else if (e->data_len == 3 && !strncasecmp(e->data, "tog", 3)) {
                cmd.op = SC_RELAY_TOGGLE;
            } else {
                break;
            }
            if (!sc_relay_submit(&cmd)) ESP_LOGW(TAG, "relay queue full");
            break;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void pubtask(void *arg)
{
    (void)arg;
    pub_t item;
    for (;;) {
        if (xQueueReceive(pq, &item, portMAX_DELAY) != pdTRUE) continue;
        if (connected && client &&
            esp_mqtt_client_publish(client, item.topic, payload_pool[item.slot], 0, item.qos, item.retain) < 0) {
            ESP_LOGW(TAG, "publish failed: %s", item.topic);
        }
        payload_release(item.slot);
    }
}

esp_err_t sc_mqtt_init(void)
{
    const sc_config_t *source = sc_config_acquire();
    cfg.mqtt_active = source->mqtt_active;
    cfg.mqtt_port = source->mqtt_port;
    cfg.mqtt_keepalive_sec = source->mqtt_keepalive_sec;
    cfg.relay_count = source->relay_count <= SC_MAX_RELAYS ? source->relay_count : SC_MAX_RELAYS;
    snprintf(cfg.mqtt_broker, sizeof(cfg.mqtt_broker), "%s", source->mqtt_broker);
    snprintf(cfg.mqtt_user, sizeof(cfg.mqtt_user), "%s", source->mqtt_user);
    snprintf(cfg.mqtt_password, sizeof(cfg.mqtt_password), "%s", source->mqtt_password);
    for (uint8_t i = 0; i < cfg.relay_count; i++) {
        snprintf(cfg.relay[i].pub_topic, sizeof(cfg.relay[i].pub_topic), "%s",
                 source->relay[i].pub_topic);
        snprintf(cfg.relay[i].state_pub_topic, sizeof(cfg.relay[i].state_pub_topic), "%s",
                 source->relay[i].state_pub_topic);
        snprintf(cfg.relay[i].sub_topic, sizeof(cfg.relay[i].sub_topic), "%s",
                 source->relay[i].sub_topic);
    }
    sc_config_release();
    if (!cfg.mqtt_active) return ESP_OK;

    pool_lock = xSemaphoreCreateMutex();
    pq = xQueueCreate(PUB_SLOTS, sizeof(pub_t));
    if (!pool_lock || !pq) return ESP_ERR_NO_MEM;
    if (xTaskCreate(pubtask, "mqpub", 1792, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(client_id, sizeof(client_id), "ESP8266-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(will_topic, sizeof(will_topic), "/home/Controller%02X%02X%02X%02X%02X%02X/Birth",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_mqtt_client_config_t mqtt_cfg = {
        .host = cfg.mqtt_broker,
        .port = cfg.mqtt_port,
        .transport = MQTT_TRANSPORT_OVER_TCP,
        .client_id = client_id,
        .username = cfg.mqtt_user[0] ? cfg.mqtt_user : NULL,
        .password = cfg.mqtt_password[0] ? cfg.mqtt_password : NULL,
        .lwt_topic = will_topic,
        .lwt_msg = "offline",
        .lwt_qos = 1,
        .lwt_retain = 1,
        .event_handle = event_cb,
        .keepalive = cfg.mqtt_keepalive_sec,
        .reconnect_timeout_ms = 30000,
        .task_stack = 3072,
        .buffer_size = 1024
    };
    client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) return ESP_FAIL;
    return esp_mqtt_client_start(client);
}

static bool queue_publish(const char *topic, const char *payload, bool retain, uint8_t qos)
{
    if (!connected || !client || !pq || !topic || !topic[0] || !payload) return false;
    if (strlen(topic) >= SC_TOPIC_LEN) {
        ESP_LOGW(TAG, "topic too long, publish skipped");
        return false;
    }
    int slot = payload_acquire(payload);
    if (slot < 0) {
        ESP_LOGW(TAG, "publish pool full: %s", topic);
        return false;
    }
    pub_t item = {.slot = (uint8_t)slot, .retain = retain, .qos = qos};
    snprintf(item.topic, sizeof(item.topic), "%s", topic);
    if (xQueueSend(pq, &item, 0) != pdTRUE) {
        payload_release(item.slot);
        ESP_LOGW(TAG, "publish queue full: %s", item.topic);
        return false;
    }
    return true;
}

bool sc_mqtt_publish(const char *topic, const char *payload, bool retain)
{
    return queue_publish(topic, payload, retain, 1);
}

bool sc_mqtt_publish_qos0(const char *topic, const char *payload, bool retain)
{
    return queue_publish(topic, payload, retain, 0);
}

void sc_mqtt_status(sc_mqtt_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = cfg.mqtt_active;
    out->connected = connected;
    out->port = cfg.mqtt_port;
    snprintf(out->server, sizeof(out->server), "%s", cfg.mqtt_broker);
    snprintf(out->health_topic, sizeof(out->health_topic), "%s", will_topic);
}

void sc_relay_state_changed(uint8_t relay, bool value)
{
    if (relay >= cfg.relay_count) return;
    const char *payload = value ? "on" : "off";
    sc_mqtt_publish(cfg.relay[relay].state_pub_topic, payload, true);
    sc_mqtt_publish(cfg.relay[relay].pub_topic, payload, true);
}
