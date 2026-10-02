#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "tcpip_adapter.h"
#include "sc_config.h"
#include "sc_health.h"
#include "sc_status_led.h"
#include "sc_wifi.h"

static sc_wifi_status_t st;
static SemaphoreHandle_t state_lock;
static TimerHandle_t retry;
static uint32_t retry_ms = 1000;
static const char *TAG = "wifi";

static void copy_field(uint8_t *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap);
    memcpy(dst, src, n);
    if (n < cap) dst[n] = 0;
}

static void copy_dns(char *dst, size_t cap, tcpip_adapter_dns_type_t type)
{
    tcpip_adapter_dns_info_t dns = {0};
    if (tcpip_adapter_get_dns_info(TCPIP_ADAPTER_IF_STA, type, &dns) == ESP_OK && !ip_addr_isany(&dns.ip))
        snprintf(dst, cap, IPSTR, IP2STR(ip_2_ip4(&dns.ip)));
    else
        dst[0] = 0;
}

static void schedule_after(uint32_t delay_ms)
{
    if (!retry) return;
    if (xTimerChangePeriod(retry, pdMS_TO_TICKS(delay_ms), 0) != pdPASS)
        ESP_LOGW(TAG, "failed to schedule Wi-Fi retry");
}

static void schedule_retry(void)
{
    schedule_after(retry_ms);
}

static void retry_cb(TimerHandle_t timer)
{
    (void)timer;
    sc_health_wifi_reconnect();
    esp_err_t err = esp_wifi_connect();
    if (retry_ms < 60000) {
        retry_ms *= 2;
        if (retry_ms > 60000) retry_ms = 60000;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect retry failed: %s", esp_err_to_name(err));
        schedule_retry();
    } else schedule_after(60000);
}

static esp_err_t evt(void *ctx, system_event_t *event)
{
    (void)ctx;
    if (event->event_id == SYSTEM_EVENT_STA_GOT_IP) {
        if (xSemaphoreTake(state_lock, portMAX_DELAY) == pdTRUE) {
            st.connected = true;
            snprintf(st.ip, sizeof(st.ip), IPSTR, IP2STR(&event->event_info.got_ip.ip_info.ip));
            snprintf(st.gateway, sizeof(st.gateway), IPSTR, IP2STR(&event->event_info.got_ip.ip_info.gw));
            copy_dns(st.dns1, sizeof(st.dns1), TCPIP_ADAPTER_DNS_MAIN);
            copy_dns(st.dns2, sizeof(st.dns2), TCPIP_ADAPTER_DNS_BACKUP);
            xSemaphoreGive(state_lock);
        }
        retry_ms = 1000;
        xTimerStop(retry, 0);
        sc_status_led_set_mode(SC_STATUS_LED_CONNECTED);
    } else if (event->event_id == SYSTEM_EVENT_STA_DISCONNECTED) {
        if (xSemaphoreTake(state_lock, portMAX_DELAY) == pdTRUE) {
            st.connected = false;
            st.ip[0] = 0;
            st.gateway[0] = 0;
            st.dns1[0] = 0;
            st.dns2[0] = 0;
            xSemaphoreGive(state_lock);
        }
        sc_status_led_set_mode(SC_STATUS_LED_CONNECTING);
        schedule_retry();
    }
    return ESP_OK;
}

esp_err_t sc_wifi_init(void)
{
    wifi_config_t sta = {0}, ap = {0};
    const sc_config_t *cfg = sc_config_acquire();
    copy_field(sta.sta.ssid, sizeof(sta.sta.ssid), cfg->ssid);
    copy_field(sta.sta.password, sizeof(sta.sta.password), cfg->wifi_password);
    copy_field(ap.ap.ssid, sizeof(ap.ap.ssid), cfg->ap_ssid);
    copy_field(ap.ap.password, sizeof(ap.ap.password), cfg->ap_password);
    sc_config_release();

    state_lock = xSemaphoreCreateMutex();
    retry = xTimerCreate("wretry", pdMS_TO_TICKS(1000), pdFALSE, NULL, retry_cb);
    if (!state_lock || !retry) return ESP_ERR_NO_MEM;

    snprintf(st.ssid, sizeof(st.ssid), "%s", (char *)sta.sta.ssid);
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(evt, NULL));
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.ssid_len = strnlen((char *)ap.ap.ssid, sizeof(ap.ap.ssid));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (sta.sta.ssid[0]) {
        ESP_LOGI(TAG, "connecting to saved SSID '%s'", (char *)sta.sta.ssid);
        sc_status_led_set_mode(SC_STATUS_LED_CONNECTING);
        if (esp_wifi_connect() != ESP_OK) schedule_retry();
        else schedule_after(60000);
    } else {
        ESP_LOGW(TAG, "station SSID is empty; setup AP '%s' is available at 192.168.4.1",
                 (char *)ap.ap.ssid);
        sc_status_led_set_mode(SC_STATUS_LED_AP_MODE);
    }
    return ESP_OK;
}

void sc_wifi_status(sc_wifi_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (state_lock && xSemaphoreTake(state_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out = st;
        xSemaphoreGive(state_lock);
    }
    if (out->connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) out->rssi = ap.rssi;
    }
}
