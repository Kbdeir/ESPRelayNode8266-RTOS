#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "sc_config.h"
#include "sc_relay.h"
#include "sc_time.h"
#include "sc_timer.h"

static sc_timer_status_t st[SC_MAX_TIMERS];
static int64_t off_deadline_us[SC_MAX_TIMERS];
static uint32_t last_min_key[SC_MAX_TIMERS];
static uint8_t active_relay[SC_MAX_TIMERS];
static bool relay_paused[SC_MAX_RELAYS];
static SemaphoreHandle_t timer_lock;

static int date_key(const char *value)
{
    int year = 0, month = 0, day = 0;
    return value && sscanf(value, "%d-%d-%d", &year, &month, &day) == 3
               ? year * 10000 + month * 100 + day
               : 0;
}

static bool date_in_range(const sc_timer_schedule_t *schedule, const struct tm *tm)
{
    int today = (tm->tm_year + 1900) * 10000 + (tm->tm_mon + 1) * 100 + tm->tm_mday;
    int from = date_key(schedule->date_from);
    int to = date_key(schedule->date_to);
    return (!from || today >= from) && (!to || today <= to);
}

static bool recurrence_matches(const sc_timer_config_t *timer, const struct tm *tm)
{
    if (timer->type == SC_TIMER_DAILY) return true;
    if (timer->type == SC_TIMER_WEEKLY) {
        int monday_zero = (tm->tm_wday + 6) % 7;
        return (timer->weekdays & (1u << monday_zero)) != 0;
    }
    if (timer->type == SC_TIMER_MONTHLY) return tm->tm_mday == timer->month_day;
    return false;
}

static bool active_occurrence(const sc_timer_config_t *timer,
                              const sc_timer_schedule_t *schedule,
                              const struct tm *now_tm, time_t now,
                              uint32_t *key, uint32_t *remaining_sec)
{
    if (!timer->enabled) return false;
    uint32_t duration = timer->duration_sec ? timer->duration_sec : 60;

    if (timer->type == SC_TIMER_ONCE) {
        uint64_t start = timer->once_epoch;
        uint64_t end = start + duration;
        if (!start || (uint64_t)now < start || (uint64_t)now >= end) return false;
        *key = (uint32_t)(start / 60);
        *remaining_sec = (uint32_t)(end - (uint64_t)now);
        return true;
    }

    int lookback_days;
    if (timer->type == SC_TIMER_DAILY) lookback_days = 1;
    else if (timer->type == SC_TIMER_WEEKLY) lookback_days = 7;
    else if (timer->type == SC_TIMER_MONTHLY) lookback_days = 62;
    else return false;

    int now_seconds = now_tm->tm_hour * 3600 + now_tm->tm_min * 60 + now_tm->tm_sec;
    int64_t midnight = (int64_t)now - now_seconds;
    int start_seconds = timer->start_hour * 3600 + timer->start_minute * 60;
    for (int days_back = 0; days_back <= lookback_days; days_back++) {
        int64_t start = midnight - (int64_t)days_back * 86400 + start_seconds;
        if (start > now) continue;
        time_t candidate = (time_t)start;
        struct tm candidate_tm;
        gmtime_r(&candidate, &candidate_tm);
        if (!date_in_range(schedule, &candidate_tm) ||
            !recurrence_matches(timer, &candidate_tm)) continue;

        uint64_t end = (uint64_t)start + duration;
        if ((uint64_t)now >= end) return false;
        *key = (uint32_t)(start / 60);
        *remaining_sec = (uint32_t)(end - (uint64_t)now);
        return true;
    }
    return false;
}

static bool relay_has_other_owner(int timer_index, uint8_t relay)
{
    for (int i = 0; i < SC_MAX_TIMERS; i++)
        if (i != timer_index && off_deadline_us[i] && active_relay[i] == relay) return true;
    return false;
}

static bool stop_timer(int timer_index)
{
    uint8_t relay = active_relay[timer_index];
    bool other_owner = relay < SC_MAX_RELAYS && relay_has_other_owner(timer_index, relay);
    if (relay < sc_relay_count() && !other_owner) {
        sc_relay_cmd_t cmd = {.relay = relay, .op = SC_RELAY_SET, .value = false};
        if (!sc_relay_submit(&cmd)) return false;
    }
    off_deadline_us[timer_index] = 0;
    active_relay[timer_index] = UINT8_MAX;
    st[timer_index].active = false;
    st[timer_index].paused = false;
    if (relay < SC_MAX_RELAYS && !other_owner) relay_paused[relay] = false;
    return true;
}

static void timer_task(void *arg)
{
    (void)arg;
    for (;;) {
        time_t now = sc_time_now_local();
        int64_t now_us = esp_timer_get_time();
        struct tm tm;
        gmtime_r(&now, &tm);
        const sc_config_t *cfg = sc_config_acquire();
        xSemaphoreTake(timer_lock, portMAX_DELAY);

        for (int i = 0; i < SC_MAX_TIMERS; i++) {
            const sc_timer_config_t *timer = &cfg->timer[i];
            const sc_timer_schedule_t *schedule = &cfg->timer_schedule[i];
            st[i].exists = timer->type != SC_TIMER_DISABLED;
            st[i].enabled = timer->enabled;
            st[i].id = i + 1;
            st[i].relay = timer->relay;
            st[i].type = timer->type;

            bool changed_while_active = off_deadline_us[i] &&
                (!timer->enabled || timer->relay != active_relay[i]);
            if (off_deadline_us[i] && (now_us >= off_deadline_us[i] || changed_while_active))
                stop_timer(i);

            uint32_t key = 0, remaining_sec = 0;
            bool valid_relay = timer->relay < cfg->relay_count || timer->relay == UINT8_MAX;
            if (sc_time_valid() && valid_relay &&
                active_occurrence(timer, schedule, &tm, now, &key, &remaining_sec) &&
                last_min_key[i] != key && !off_deadline_us[i]) {
                bool started = true;
                if (timer->relay < cfg->relay_count) {
                    sc_relay_cmd_t cmd = {.relay = timer->relay, .op = SC_RELAY_SET, .value = true};
                    started = sc_relay_submit(&cmd);
                }
                if (started) {
                    last_min_key[i] = key;
                    active_relay[i] = timer->relay;
                    if (timer->relay < SC_MAX_RELAYS) relay_paused[timer->relay] = false;
                    off_deadline_us[i] = now_us + (int64_t)remaining_sec * 1000000LL;
                    st[i].active = true;
                }
            }

            st[i].active = off_deadline_us[i] != 0;
            st[i].paused = st[i].active && active_relay[i] < SC_MAX_RELAYS &&
                           relay_paused[active_relay[i]];
            if (off_deadline_us[i]) {
                int64_t remaining = off_deadline_us[i] - now_us;
                st[i].seconds_to_off = remaining > 0 ? (int32_t)((remaining + 999999) / 1000000) : 0;
            } else {
                st[i].seconds_to_off = -1;
            }
            st[i].seconds_to_on = -1;
        }

        xSemaphoreGive(timer_lock);
        sc_config_release();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t sc_timer_init(void)
{
    memset(st, 0, sizeof(st));
    memset(active_relay, UINT8_MAX, sizeof(active_relay));
    timer_lock = xSemaphoreCreateMutex();
    if (!timer_lock) return ESP_ERR_NO_MEM;
    return xTaskCreate(timer_task, "schedule", 2048, NULL, 4, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

void sc_timer_status(uint8_t id, sc_timer_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (id < 1 || id > SC_MAX_TIMERS || !timer_lock) return;
    if (xSemaphoreTake(timer_lock, portMAX_DELAY) == pdTRUE) {
        *out = st[id - 1];
        xSemaphoreGive(timer_lock);
    }
}

bool sc_timer_set_paused(uint8_t relay, bool paused)
{
    if (relay >= SC_MAX_RELAYS || !timer_lock) return false;
    if (xSemaphoreTake(timer_lock, portMAX_DELAY) != pdTRUE) return false;
    bool active = false;
    for (int i = 0; i < SC_MAX_TIMERS; i++)
        if (active_relay[i] == relay && off_deadline_us[i]) active = true;
    if (active && relay_paused[relay] != paused) {
        sc_relay_cmd_t cmd = {.relay = relay, .op = SC_RELAY_SET, .value = !paused};
        if (sc_relay_submit(&cmd)) relay_paused[relay] = paused;
    }
    for (int i = 0; i < SC_MAX_TIMERS; i++)
        if (active_relay[i] == relay && off_deadline_us[i]) st[i].paused = relay_paused[relay];
    bool result = active && relay_paused[relay];
    xSemaphoreGive(timer_lock);
    return result;
}
