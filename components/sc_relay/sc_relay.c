#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sc_config.h"
#include "sc_health.h"
#include "sc_relay.h"
#define RELAY_TIMER_POLL_MS 10
typedef struct {
    uint8_t gpio;
    uint8_t active_high;
    uint32_t ttl_sec;
    uint32_t tta_sec;
} relay_runtime_config_t;
static QueueHandle_t q;
static bool state[SC_MAX_RELAYS];
static int64_t ttl_deadline_us[SC_MAX_RELAYS];
static int64_t tta_deadline_us[SC_MAX_RELAYS];
static relay_runtime_config_t relay_cfg[SC_MAX_RELAYS];
static uint8_t relay_count;
static const char *TAG = "relay";
__attribute__((weak)) void sc_inputs_poll(void) {}
__attribute__((weak)) void sc_relay_state_changed(uint8_t relay, bool value)
{
    (void)relay;
    (void)value;
}
static void write_relay(uint8_t n, bool v)
{
    if (n >= relay_count)
        return;
    bool changed = state[n] != v;
    gpio_set_level(relay_cfg[n].gpio, relay_cfg[n].active_high ? v : !v);
    state[n] = v;
    if (changed)
        sc_relay_state_changed(n, v);
}
static int64_t deadline_after(int64_t now_us, uint32_t seconds)
{
    return seconds ? now_us + (int64_t)seconds * 1000000LL : 0;
}
static void set_deadlines(uint8_t n, int64_t ttl, int64_t tta)
{
    taskENTER_CRITICAL();
    ttl_deadline_us[n] = ttl;
    tta_deadline_us[n] = tta;
    taskEXIT_CRITICAL();
}
static void set_relay_now(uint8_t n, bool value, int64_t now_us)
{
    set_deadlines(n, value ? deadline_after(now_us, relay_cfg[n].ttl_sec) : 0, 0);
    write_relay(n, value);
}
static void request_relay_on(uint8_t n, int64_t now_us)
{
    if (state[n]) {
        set_relay_now(n, true, now_us);
        return;
    }
    if (tta_deadline_us[n]) return;
    if (!relay_cfg[n].tta_sec) {
        set_relay_now(n, true, now_us);
        return;
    }
    set_deadlines(n, 0, deadline_after(now_us, relay_cfg[n].tta_sec));
    ESP_LOGI(TAG, "relay %u activation delayed by %u second(s)",
             (unsigned)n, (unsigned)relay_cfg[n].tta_sec);
}
static void handle_command(const sc_relay_cmd_t *c, int64_t now_us)
{
    uint8_t n = c->relay;
    if (c->op == SC_RELAY_SET) {
        if (c->value) request_relay_on(n, now_us);
        else set_relay_now(n, false, now_us);
        return;
    }

    if (state[n] || tta_deadline_us[n]) {
        set_relay_now(n, false, now_us);
        return;
    }
    request_relay_on(n, now_us);
}
static void process_deadlines(int64_t now_us)
{
    for (uint8_t n = 0; n < relay_count; n++) {
        int64_t ttl, tta;
        taskENTER_CRITICAL();
        ttl = ttl_deadline_us[n];
        tta = tta_deadline_us[n];
        taskEXIT_CRITICAL();
        if (tta && now_us >= tta) {
            ESP_LOGI(TAG, "relay %u TTA expired, switching on", (unsigned)n);
            set_relay_now(n, true, now_us);
        } else if (ttl && now_us >= ttl) {
            ESP_LOGI(TAG, "relay %u TTL expired, switching off", (unsigned)n);
            set_relay_now(n, false, now_us);
        }
    }
}
static void task(void *p)
{
    (void)p;
    sc_relay_cmd_t c;
    for (;;)
    {
        if (xQueueReceive(q, &c, pdMS_TO_TICKS(RELAY_TIMER_POLL_MS)) == pdTRUE &&
            c.relay < relay_count)
        {
            handle_command(&c, esp_timer_get_time());
            sc_health_relay_command();
        }
        process_deadlines(esp_timer_get_time());
        sc_inputs_poll();
    }
}
esp_err_t sc_relay_init(void)
{
    const sc_config_t *cfg = sc_config_acquire();
    relay_count = cfg->relay_count <= SC_MAX_RELAYS ? cfg->relay_count : SC_MAX_RELAYS;
    for (int i = 0; i < relay_count; i++) {
        relay_cfg[i].gpio = cfg->relay[i].gpio;
        relay_cfg[i].active_high = cfg->relay[i].active_high;
        relay_cfg[i].ttl_sec = cfg->relay[i].ttl_sec;
        relay_cfg[i].tta_sec = cfg->relay[i].tta_sec;
    }
    sc_config_release();
    for (int i = 0; i < relay_count; i++)
    {
        gpio_config_t g = {.pin_bit_mask = (1ULL << relay_cfg[i].gpio), .mode = GPIO_MODE_OUTPUT};
        gpio_config(&g);
        write_relay(i, false);
    }
    q = xQueueCreate(12, sizeof(sc_relay_cmd_t));
    if (!q)
        return ESP_ERR_NO_MEM;
    return xTaskCreate(task, "relay", 2560, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
bool sc_relay_submit(const sc_relay_cmd_t *c) { return q && c && xQueueSend(q, c, pdMS_TO_TICKS(50)) == pdTRUE; }
bool sc_relay_get(uint8_t n) { return n < SC_MAX_RELAYS ? state[n] : false; }
bool sc_relay_status(uint8_t n, sc_relay_status_t *out)
{
    if (!out || n >= relay_count) return false;
    int64_t ttl, tta;
    taskENTER_CRITICAL();
    ttl = ttl_deadline_us[n];
    tta = tta_deadline_us[n];
    out->state = state[n];
    taskEXIT_CRITICAL();
    int64_t now = esp_timer_get_time();
    out->ttl_active = ttl > now;
    out->tta_active = tta > now;
    out->ttl_remaining_sec = out->ttl_active ? (uint32_t)((ttl - now + 999999) / 1000000) : 0;
    out->tta_remaining_sec = out->tta_active ? (uint32_t)((tta - now + 999999) / 1000000) : 0;
    return true;
}
bool sc_relay_set_timing(uint8_t n, bool ttl, uint32_t seconds)
{
    if (n >= relay_count) return false;
    int64_t now = esp_timer_get_time();
    bool activate = false;
    taskENTER_CRITICAL();
    if (ttl) {
        relay_cfg[n].ttl_sec = seconds;
        if (state[n])
            ttl_deadline_us[n] = deadline_after(now, seconds);
    } else {
        relay_cfg[n].tta_sec = seconds;
        if (tta_deadline_us[n]) {
            tta_deadline_us[n] = deadline_after(now, seconds);
            activate = seconds == 0;
        }
    }
    taskEXIT_CRITICAL();
    if (activate) {
        sc_relay_cmd_t command = {
            .relay = n,
            .op = SC_RELAY_SET,
            .value = true,
        };
        sc_relay_submit(&command);
    }
    return true;
}
uint8_t sc_relay_count(void) { return relay_count; }
