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

#define PUB_SLOTS 4
#define PUB_PAYLOAD_LEN 672

typedef enum {
    MQTT_ITEM_PUBLISH = 0,
    MQTT_ITEM_TIMING = 1,
} mqtt_item_kind_t;

typedef struct {
    char topic[SC_TOPIC_LEN];
    uint32_t value;
    uint8_t slot;
    uint8_t retain;
    uint8_t qos;
    uint8_t kind;
    uint8_t relay;
    uint8_t timing_ttl;
} pub_t;

typedef struct {
    uint8_t mqtt_active;
    char mqtt_broker[SC_HOST_LEN];
    uint16_t mqtt_port;
    uint16_t mqtt_keepalive_sec;
    char mqtt_user[SC_USER_LEN];
    char mqtt_password[SC_PASS_LEN];
    uint8_t relay_count;
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
static volatile bool connection_sync_pending;
static volatile bool timing_commands_ready;
static volatile uint8_t timing_sync_mask;
static volatile uint8_t relay_state_sync_mask;
static volatile uint8_t relay_state_values;
static volatile bool network_ready;
static volatile bool client_started;
static volatile bool client_starting;
static const char *TAG = "mqtt";
static esp_err_t start_client_if_ready(void);
__attribute__((weak)) bool sc_automation_remote_message(
    const char *topic, size_t topic_len, const char *payload, size_t payload_len)
{
    (void)topic;
    (void)topic_len;
    (void)payload;
    (void)payload_len;
    return false;
}
__attribute__((weak)) void sc_ha_mqtt_connected(void) {}

static void subscribe_remote_sensors(void)
{
    if (!connected || !client) return;
    for (uint8_t i = 0; i < SC_MAX_REMOTE_SENSORS; i++) {
        char topic[SC_REMOTE_TOPIC_LEN];
        const sc_config_t *source = sc_config_acquire();
        snprintf(topic, sizeof(topic), "%s",
                 source->remote_transport[i] == SC_REMOTE_MQTT
                     ? source->remote_sensor[i].topic : "");
        sc_config_release();
        if (topic[0]) {
            esp_mqtt_client_subscribe(client, topic, 0);
            ESP_LOGI(TAG, "remote sensor %u subscribed: %s",
                     (unsigned)(i + 1), topic);
        }
    }
}

static int topic_eq(esp_mqtt_event_handle_t e, const char *topic)
{
    size_t n = strlen(topic);
    return e->topic_len == (int)n && !strncmp(e->topic, topic, n);
}

static bool parse_seconds(const char *payload, int length, uint32_t *out)
{
    if (!payload || !out || length <= 0 || length > 10) return false;
    uint64_t value = 0;
    for (int i = 0; i < length; i++) {
        unsigned char c = (unsigned char)payload[i];
        if (c < '0' || c > '9') return false;
        value = value * 10U + (uint32_t)(c - '0');
        if (value > UINT32_MAX) return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool queue_timing_update(uint8_t relay, bool ttl, uint32_t seconds)
{
    if (!pq) return false;
    pub_t item = {
        .value = seconds,
        .kind = MQTT_ITEM_TIMING,
        .relay = relay,
        .timing_ttl = ttl,
    };
    return xQueueSend(pq, &item, pdMS_TO_TICKS(50)) == pdTRUE;
}

static int payload_acquire(const char *payload)
{
    size_t n = strlen(payload) + 1;
    if (n > PUB_PAYLOAD_LEN) {
        ESP_LOGW(TAG, "payload too large: %u bytes", (unsigned)n);
        return -1;
    }
    if (!pool_lock) return -1;
    TickType_t started = xTaskGetTickCount();
    const TickType_t wait = pdMS_TO_TICKS(250);
    do {
        if (xSemaphoreTake(pool_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            for (int i = 0; i < PUB_SLOTS; i++) {
                if (!payload_used[i]) {
                    payload_used[i] = true;
                    memcpy(payload_pool[i], payload, n);
                    xSemaphoreGive(pool_lock);
                    return i;
                }
            }
            xSemaphoreGive(pool_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    } while ((TickType_t)(xTaskGetTickCount() - started) < wait);
    return -1;
}

static void payload_release(uint8_t slot)
{
    if (slot >= PUB_SLOTS || !pool_lock) return;
    if (xSemaphoreTake(pool_lock, portMAX_DELAY) == pdTRUE) {
        payload_used[slot] = false;
        xSemaphoreGive(pool_lock);
    }
}

static bool publish_direct(const char *topic, const char *payload, int qos)
{
    if (!connected || !client || !topic || !topic[0] || !payload) return false;
    if (esp_mqtt_client_publish(client, topic, payload, 0, qos, true) < 0) {
        ESP_LOGW(TAG, "direct publish failed: %s", topic);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(25));
    return true;
}

static bool publish_timing_direct(uint8_t relay, bool publish_commands,
                                  bool subscribe_after)
{
    char pub_topic[SC_TOPIC_LEN];
    char topic[SC_TOPIC_LEN];
    char value[12];
    uint32_t ttl, tta;
    const sc_config_t *source = sc_config_acquire();
    if (relay >= source->relay_count) {
        sc_config_release();
        return false;
    }
    snprintf(pub_topic, sizeof(pub_topic), "%s", source->relay[relay].pub_topic);
    ttl = source->relay[relay].ttl_sec;
    tta = source->relay[relay].tta_sec;
    sc_config_release();

    const uint32_t values[2] = {ttl, tta};
    const char *names[2] = {"TTL", "TTA"};
    bool ok = true;
    for (int i = 0; i < 2; i++) {
        snprintf(value, sizeof(value), "%u", (unsigned)values[i]);
        int n = snprintf(topic, sizeof(topic), "%s/%s", pub_topic, names[i]);
        if (n <= 0 || n >= (int)sizeof(topic)) {
            ok = false;
            continue;
        }
        ok = publish_direct(topic, value, 1) && ok;
        if (!publish_commands) continue;
        n = snprintf(topic, sizeof(topic), "%s/%s/set", pub_topic, names[i]);
        if (n <= 0 || n >= (int)sizeof(topic)) {
            ok = false;
            continue;
        }
        ok = publish_direct(topic, value, 1) && ok;
        if (subscribe_after)
            esp_mqtt_client_subscribe(client, topic, 1);
    }
    return ok;
}

static bool publish_relay_state_direct(uint8_t relay, bool value,
                                       bool publish_command)
{
    char state_topic[SC_TOPIC_LEN];
    char pub_topic[SC_TOPIC_LEN];
    char command_topic[SC_TOPIC_LEN];
    const sc_config_t *source = sc_config_acquire();
    if (relay >= source->relay_count) {
        sc_config_release();
        return false;
    }
    snprintf(state_topic, sizeof(state_topic), "%s",
             source->relay[relay].state_pub_topic);
    snprintf(pub_topic, sizeof(pub_topic), "%s",
             source->relay[relay].pub_topic);
    snprintf(command_topic, sizeof(command_topic), "%s",
             source->relay[relay].sub_topic);
    sc_config_release();
    const char *payload = value ? "on" : "off";
    bool ok = publish_direct(state_topic, payload, 1);
    ok = publish_direct(pub_topic, payload, 1) && ok;
    if (publish_command)
        ok = publish_direct(command_topic, payload, 1) && ok;
    return ok;
}

static void publish_connected_snapshot(void)
{
    timing_commands_ready = false;
    for (uint8_t i = 0; connected && i < cfg.relay_count; i++) {
        char sub_topic[SC_TOPIC_LEN];
        const sc_config_t *source = sc_config_acquire();
        snprintf(sub_topic, sizeof(sub_topic), "%s",
                 source->relay[i].sub_topic);
        sc_config_release();
        esp_mqtt_client_subscribe(client, sub_topic, 1);
        /* Leave the retained command untouched until the broker delivers it. */
        publish_relay_state_direct(i, sc_relay_get(i), false);
        publish_timing_direct(i, true, true);
    }
    if (!connected) return;
    subscribe_remote_sensors();
    publish_direct(will_topic, "online", 1);
    timing_commands_ready = true;
    sc_ha_mqtt_connected();
}

static esp_err_t event_cb(esp_mqtt_event_handle_t e)
{
    switch (e->event_id) {
    case MQTT_EVENT_CONNECTED:
        connected = true;
        timing_commands_ready = false;
        connection_sync_pending = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        connected = false;
        timing_commands_ready = false;
        sc_health_mqtt_reconnect();
        break;
    case MQTT_EVENT_DATA:
        for (int i = 0; i < cfg.relay_count; i++) {
            char pub_topic[SC_TOPIC_LEN];
            char timing_topic[SC_TOPIC_LEN];
            const sc_config_t *source = sc_config_acquire();
            snprintf(pub_topic, sizeof(pub_topic), "%s",
                     source->relay[i].pub_topic);
            sc_config_release();
            int topic_len = snprintf(timing_topic, sizeof(timing_topic),
                                     "%s/TTL/set", pub_topic);
            bool ttl = topic_len > 0 && topic_len < (int)sizeof(timing_topic) &&
                       topic_eq(e, timing_topic);
            topic_len = snprintf(timing_topic, sizeof(timing_topic),
                                 "%s/TTA/set", pub_topic);
            bool tta = topic_len > 0 && topic_len < (int)sizeof(timing_topic) &&
                       topic_eq(e, timing_topic);
            if (!ttl && !tta) continue;
            if (!timing_commands_ready) {
                ESP_LOGW(TAG, "ignored retained timing command during reconnect sync");
                return ESP_OK;
            }
            uint32_t seconds;
            if (!parse_seconds(e->data, e->data_len, &seconds)) {
                ESP_LOGW(TAG, "invalid relay %d %s payload", i,
                         ttl ? "TTL" : "TTA");
            } else if (!queue_timing_update((uint8_t)i, ttl, seconds)) {
                ESP_LOGW(TAG, "timing update queue full");
            }
            return ESP_OK;
        }
        for (int i = 0; i < cfg.relay_count; i++) {
            char sub_topic[SC_TOPIC_LEN];
            const sc_config_t *source = sc_config_acquire();
            snprintf(sub_topic, sizeof(sub_topic), "%s",
                     source->relay[i].sub_topic);
            sc_config_release();
            if (!topic_eq(e, sub_topic)) continue;
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
        sc_automation_remote_message(e->topic, (size_t)e->topic_len,
                                     e->data, (size_t)e->data_len);
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
        if (connection_sync_pending) {
            connection_sync_pending = false;
            if (connected) publish_connected_snapshot();
            continue;
        }
        uint8_t sync_mask, state_mask, state_values;
        taskENTER_CRITICAL();
        sync_mask = timing_sync_mask;
        timing_sync_mask = 0;
        state_mask = relay_state_sync_mask;
        relay_state_sync_mask = 0;
        state_values = relay_state_values;
        taskEXIT_CRITICAL();
        for (uint8_t relay = 0; connected && relay < cfg.relay_count; relay++)
            if (sync_mask & (1U << relay))
                publish_timing_direct(relay, true, true);
        for (uint8_t relay = 0; connected && relay < cfg.relay_count; relay++)
            if (state_mask & (1U << relay))
                publish_relay_state_direct(
                    relay, (state_values & (1U << relay)) != 0, true);
        if (xQueueReceive(pq, &item, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (item.kind == MQTT_ITEM_TIMING) {
            bool changed = false;
            const sc_config_t *source = sc_config_acquire();
            if (item.relay < source->relay_count)
                changed = item.timing_ttl
                    ? source->relay[item.relay].ttl_sec != item.value
                    : source->relay[item.relay].tta_sec != item.value;
            sc_config_release();
            esp_err_t err = sc_config_set_relay_timing(
                item.relay, item.timing_ttl, item.value);
            if (err == ESP_OK && item.relay < cfg.relay_count) {
                if (changed) {
                    sc_relay_set_timing(item.relay, item.timing_ttl,
                                        item.value);
                }
                publish_timing_direct(item.relay, false, false);
                if (changed)
                    ESP_LOGI(TAG, "relay %u %s set to %u second(s)",
                             (unsigned)item.relay,
                             item.timing_ttl ? "TTL" : "TTA",
                             (unsigned)item.value);
            } else {
                ESP_LOGE(TAG, "relay timing update failed: %s",
                         esp_err_to_name(err));
                publish_timing_direct(item.relay, false, false);
            }
            continue;
        }
        if (connected && client &&
            esp_mqtt_client_publish(client, item.topic, payload_pool[item.slot], 0, item.qos, item.retain) < 0) {
            ESP_LOGW(TAG, "publish failed: %s", item.topic);
        }
        payload_release(item.slot);
    }
}

static esp_err_t start_client_if_ready(void)
{
    taskENTER_CRITICAL();
    bool start = network_ready && client && !client_started && !client_starting;
    if (start) client_starting = true;
    taskEXIT_CRITICAL();
    if (!start) return ESP_OK;

    esp_err_t err = esp_mqtt_client_start(client);
    taskENTER_CRITICAL();
    client_started = err == ESP_OK;
    client_starting = false;
    taskEXIT_CRITICAL();
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Wi-Fi has an IP; MQTT client started");
    else
        ESP_LOGE(TAG, "could not start MQTT client: %s", esp_err_to_name(err));
    return err;
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
        .reconnect_timeout_ms = 2000,
        .task_stack = 3072,
        .buffer_size = 1024
    };
    client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) return ESP_FAIL;
    if (!network_ready) {
        ESP_LOGI(TAG, "waiting for Wi-Fi IP before starting MQTT");
        return ESP_OK;
    }
    return start_client_if_ready();
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
    pub_t item = {.slot = (uint8_t)slot, .retain = retain, .qos = qos,
                  .kind = MQTT_ITEM_PUBLISH};
    snprintf(item.topic, sizeof(item.topic), "%s", topic);
    if (xQueueSend(pq, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
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

bool sc_mqtt_connected(void)
{
    return connected;
}

void sc_mqtt_network_ready(void)
{
    network_ready = true;
    if (!client_started) {
        start_client_if_ready();
        return;
    }
    if (!client || connected) return;
    esp_err_t err = esp_mqtt_client_reconnect(client);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Wi-Fi is ready; reconnecting to broker now");
}

void sc_mqtt_network_down(void)
{
    network_ready = false;
    connected = false;
    timing_commands_ready = false;
}

void sc_relay_state_changed(uint8_t relay, bool value)
{
    if (relay >= cfg.relay_count) return;
    uint8_t bit = (uint8_t)(1U << relay);
    taskENTER_CRITICAL();
    if (value) relay_state_values |= bit;
    else relay_state_values &= (uint8_t)~bit;
    relay_state_sync_mask |= bit;
    taskEXIT_CRITICAL();
}

void sc_mqtt_update_remote_subscription(const char *old_topic,
                                        const char *new_topic)
{
    if (!connected || !client) return;
    bool changed = !old_topic || !new_topic || strcmp(old_topic, new_topic);
    if (!changed) return;
    if (old_topic && old_topic[0])
        esp_mqtt_client_unsubscribe(client, old_topic);
    if (new_topic && new_topic[0])
        esp_mqtt_client_subscribe(client, new_topic, 0);
}

void sc_mqtt_relay_config_saved(uint8_t relay, const char *old_sub_topic,
                                const char *old_pub_topic)
{
    if (relay >= cfg.relay_count) return;
    if (connected && client) {
        char topic[SC_TOPIC_LEN];
        if (old_sub_topic && old_sub_topic[0])
            esp_mqtt_client_unsubscribe(client, old_sub_topic);
        if (old_pub_topic && old_pub_topic[0]) {
            int n = snprintf(topic, sizeof(topic), "%s/TTL/set", old_pub_topic);
            if (n > 0 && n < (int)sizeof(topic))
                esp_mqtt_client_unsubscribe(client, topic);
            n = snprintf(topic, sizeof(topic), "%s/TTA/set", old_pub_topic);
            if (n > 0 && n < (int)sizeof(topic))
                esp_mqtt_client_unsubscribe(client, topic);
        }
        char new_sub_topic[SC_TOPIC_LEN];
        const sc_config_t *source = sc_config_acquire();
        snprintf(new_sub_topic, sizeof(new_sub_topic), "%s",
                 source->relay[relay].sub_topic);
        sc_config_release();
        if (new_sub_topic[0])
            esp_mqtt_client_subscribe(client, new_sub_topic, 1);
        taskENTER_CRITICAL();
        timing_sync_mask |= (uint8_t)(1U << relay);
        if (sc_relay_get(relay)) relay_state_values |= (uint8_t)(1U << relay);
        else relay_state_values &= (uint8_t)~(1U << relay);
        relay_state_sync_mask |= (uint8_t)(1U << relay);
        taskEXIT_CRITICAL();
    }
}
