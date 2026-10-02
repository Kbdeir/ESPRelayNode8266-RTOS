#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/apps/sntp.h"
#include "sc_config.h"
#include "sc_wifi.h"
#include "sc_time.h"

static int8_t timezone_hours;
static char host[SC_HOST_LEN];

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        sc_wifi_status_t wifi;
        sc_wifi_status(&wifi);
        if (wifi.connected) {
            sntp_setoperatingmode(SNTP_OPMODE_POLL);
            sntp_setservername(0, host[0] ? host : "pool.ntp.org");
            sntp_init();
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t sc_time_init(void)
{
    const sc_config_t *cfg = sc_config_acquire();
    timezone_hours = cfg->timezone_hours;
    snprintf(host, sizeof(host), "%s", cfg->timeserver);
    sc_config_release();
    return xTaskCreate(task, "sntp", 1536, NULL, 2, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool sc_time_valid(void)
{
    return time(NULL) > 1600000000;
}

time_t sc_time_now_local(void)
{
    return time(NULL) + (time_t)timezone_hours * 3600;
}

void sc_time_format(char *out, unsigned cap)
{
    if (!out || !cap) return;
    if (!sc_time_valid()) {
        snprintf(out, cap, "not synced");
        return;
    }
    time_t now = sc_time_now_local();
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm);
}
