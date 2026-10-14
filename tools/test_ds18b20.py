"""Exercise production sensor logic with mocked RTOS and 1-Wire hardware.

Uses cc on POSIX or the default WSL distribution on Windows.
"""

import os
from pathlib import Path
import subprocess
import tempfile

PROJECT_DIR = Path(__file__).resolve().parent.parent

TIMING_MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define IRAM_ATTR
static struct { uint32_t out_w1tc, enable_w1ts, enable_w1tc, in; } GPIO;
static uint32_t clock_ticks, origin, g_esp_ticks_per_us = 1;
static int critical_depth, bus_mode;
#define taskENTER_CRITICAL() do { assert(!critical_depth); critical_depth++; } while (0)
#define taskEXIT_CRITICAL() do { assert(critical_depth == 1); critical_depth--; } while (0)
static uint32_t soc_get_ccount(void) {
    uint32_t elapsed = clock_ticks - origin;
    bool high = bus_mode == 0 ||
                (bus_mode == 1 && (elapsed < 480 || elapsed > 720)) ||
                (bus_mode == 2 && elapsed > 15);
    GPIO.in = high ? (1U << 14) : 0;
    return clock_ticks++;
}
'''

TIMING_CHECKS = r'''
static void begin_slot(int mode, uint32_t start) {
    bus_mode = mode; clock_ticks = origin = start;
    GPIO.out_w1tc = GPIO.enable_w1ts = GPIO.enable_w1tc = 0;
}
int main(void) {
    begin_slot(1, 0);
    assert(ow_reset(14));
    assert(clock_ticks - origin >= 960 && clock_ticks - origin <= 970);
    assert(GPIO.out_w1tc == (1U << 14) && GPIO.enable_w1tc == (1U << 14));
    begin_slot(0, 0);
    assert(!ow_reset(14));
    begin_slot(3, 0);
    assert(!ow_reset(14)); // A shorted bus must not count as a sensor.
    begin_slot(2, 0);
    assert(ow_read_bit(14) == 0); // Sample before the 15 us deadline.
    assert(clock_ticks >= 70 && clock_ticks <= 76);
    begin_slot(0, UINT32_MAX - 5);
    assert(ow_read_bit(14) == 1);
    assert(clock_ticks - origin >= 70 && clock_ticks - origin <= 76);
    for (int bit = 0; bit < 2; bit++) {
        begin_slot(0, 0);
        ow_write_bit(14, bit);
        assert(clock_ticks >= 70 && clock_ticks <= 76);
        assert(GPIO.out_w1tc == (1U << 14));
        assert(GPIO.enable_w1ts == (1U << 14) && GPIO.enable_w1tc == (1U << 14));
    }
    begin_slot(0, 0);
    ow_write(14, 0xCC);
    assert(clock_ticks >= 8 * 70);
    begin_slot(0, 0);
    assert(ow_read(14) == 0xFF);
    assert(!critical_depth);
    puts("PASS: production 1-Wire reset/presence, stuck bus, read sample window, "
         "write slots, byte transfers and cycle-counter wrap");
    return 0;
}
'''

MOCKS = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sc_sensors.h"
#include "sc_relay.h"
#include "sc_format.h"
typedef struct { int mode, clk_div; } adc_config_t;
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
#define ADC_READ_TOUT_MODE 0
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define pdPASS 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)
static sc_config_t config;
static bool present[17], complete[17];
static uint8_t scratch[17][9], read_offset[17];
static uint32_t gpio_mask;
static int publications, relay_calls;
static int gpio_result = ESP_OK;
static int wait_ms;
static char payload[2][24];
static int64_t now_us = 1000000;
static int critical_depth;
#define taskENTER_CRITICAL() do { assert(!critical_depth); critical_depth++; } while (0)
#define taskEXIT_CRITICAL() do { assert(critical_depth == 1); critical_depth--; } while (0)

const sc_config_t *sc_config_acquire(void) { return &config; }
void sc_config_release(void) {}
static void adc_init(const adc_config_t *cfg) { (void)cfg; }
static int64_t esp_timer_get_time(void) { return now_us; }
static int gpio_config(const gpio_config_t *cfg) {
    assert(cfg->mode == GPIO_MODE_INPUT && cfg->pull_up_en == GPIO_PULLUP_ENABLE);
    assert(cfg->pull_down_en == GPIO_PULLDOWN_DISABLE);
    if (gpio_result == ESP_OK) gpio_mask |= (uint32_t)cfg->pin_bit_mask;
    return gpio_result;
}
static int ow_reset(uint8_t pin) { return present[pin]; }
static int ow_read_bit(uint8_t pin) { return complete[pin]; }
static void ow_write(uint8_t pin, uint8_t value) {
    assert(value == 0xCC || value == 0x44 || value == 0xBE);
    if (value == 0x44) complete[pin] = false;
    if (value == 0xBE) read_offset[pin] = 0;
}
static uint8_t ow_read(uint8_t pin) {
    assert(read_offset[pin] < 9);
    return scratch[pin][read_offset[pin]++];
}
static float acs_read_rms(uint8_t model, uint16_t zero) {
    (void)model; (void)zero; return 0;
}
static void sc_mqtt_publish_qos0(const char *topic, const char *value, bool retain) {
    assert(retain);
    int index = !strcmp(topic, "tank") ? 0 : 1;
    assert(index == 0 || !strcmp(topic, "panel"));
    snprintf(payload[index], sizeof(payload[index]), "%s", value);
    publications++;
}
bool sc_relay_submit(const sc_relay_cmd_t *command) {
    assert(command->relay == 0 && command->op == SC_RELAY_SET);
    relay_calls++; return true;
}
'''

WEB_MOCKS = r'''
#include "sc_health.h"
#define HTTPD_500 "500 Internal Server Error"
typedef struct { char body[2048]; size_t used; bool fail_send, finished; } httpd_req_t;
static void httpd_resp_set_type(httpd_req_t *r, const char *type) { (void)r; (void)type; }
static void httpd_resp_set_hdr(httpd_req_t *r, const char *key, const char *value) {
    (void)r; (void)key; (void)value;
}
static esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const void *data, size_t n) {
    if (r->fail_send) return ESP_FAIL;
    if (!data) { r->finished = true; return ESP_OK; }
    assert(n <= 128 && r->used + n < sizeof(r->body));
    memcpy(r->body + r->used, data, n);
    r->used += n; r->body[r->used] = 0;
    return ESP_OK;
}
void sc_health_snapshot(sc_health_snapshot_t *out) { memset(out, 0, sizeof(*out)); }
uint8_t sc_relay_count(void) { return config.relay_count; }
bool sc_relay_get(uint8_t relay) { (void)relay; return false; }
static void sc_time_format(char *out, size_t size) { snprintf(out, size, "test time"); }
static void uptime_string(char *out, size_t size) { snprintf(out, size, "test uptime"); }
static esp_err_t json_text(httpd_req_t *r, const char *body) {
    snprintf(r->body, sizeof(r->body), "%s", body); return ESP_OK;
}
static esp_err_t send_error(httpd_req_t *r, const char *status, const char *message) {
    (void)r; (void)status; (void)message; return ESP_FAIL;
}
'''

CHECKS = r'''
static uint8_t reference_crc(const uint8_t *data) {
    uint8_t crc = 0;
    for (int i = 0; i < 8; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0x8C : 0);
    }
    return crc;
}
static void set_temperature(int pin, int16_t raw, uint8_t resolution) {
    uint8_t data[9] = {(uint8_t)raw, (uint8_t)((uint16_t)raw >> 8),
                       0x4B, 0x46, resolution, 0xFF, 0x0C, 0x10, 0};
    data[8] = reference_crc(data);
    memcpy(scratch[pin], data, sizeof(data));
    present[pin] = complete[pin] = true;
}
static void run_iteration(void) {
    publications = relay_calls = wait_ms = 0;
    memset(payload, 0, sizeof(payload));
    now_us += 1500000;
    sc_sensors_poll();
    if (converting) {
        assert(publications == 0 && conversion_ready_us - now_us >= 750000);
        sc_sensors_poll();
        assert(publications == 0); // Polling never blocks waiting for a conversion.
        now_us += 759000;
        sc_sensors_poll();
        assert(converting && publications == 0);
        now_us += 1000;
        wait_ms = 760;
        for (int pin = 0; pin < 17; pin++) complete[pin] = true;
        sc_sensors_poll();
        assert(!converting);
    }
    assert(!critical_depth);
}
int main(void) {
    config.ds18b20_gpio = 14; config.ds18b20_second_gpio = 12;
    config.relay_count = 1; config.relay[0].gpio = 5;
    strcpy(config.temp_topic1, "tank"); strcpy(config.temp_topic2, "panel");
    assert(sc_sensors_init() == ESP_OK);
    sc_sensor_snapshot_t result;
    sc_sensors_snapshot(&result);
    assert(!result.temp1_valid && !result.temp2_valid);
    config.ds18b20_active = 1;
    assert(sc_sensors_validate_config(&config));
    assert(sc_sensors_ensure_started() == ESP_OK);
    assert(sc_sensors_ensure_started() == ESP_OK);
    assert(!sc_sensors_validate_config(NULL));
    for (int pin = 0; pin < 256; pin++) {
        config.ds18b20_gpio = pin;
        bool expected = pin == 0 || pin == 2 || pin == 4 || (pin >= 12 && pin <= 15);
        assert(sc_sensors_validate_config(&config) == expected);
    }
    config.ds18b20_gpio = 14;
    config.input_count = 1; config.input[0].gpio = 14;
    config.input[0].mode = SC_INPUT_COPY_TO_RELAY;
    assert(!sc_sensors_validate_config(&config));
    set_temperature(14, 401, 0x7F);
    set_temperature(12, -162, 0x7F);
    run_iteration();
    sc_sensors_snapshot(&result);
    assert(result.temp1_valid && result.temp2_valid);
    assert(result.temp1_c == 25.0625f && result.temp2_c == -10.125f);
    config.input[0].mode = SC_INPUT_TEMPERATURE;
    assert(sc_sensors_validate_config(&config));

    // DS18B20 datasheet power-on scratchpad and its known CRC byte.
    const uint8_t power_on[9] = {0x50,0x05,0x4B,0x46,0x7F,0xFF,0x0C,0x10,0x1C};
    memcpy(scratch[14], power_on, sizeof(power_on));
    present[14] = complete[14] = true;
    float value = -999;
    assert(ds_read(14, &value) && value == 85.0f);
    assert(read_offset[14] == 9);
    set_temperature(14, 401, 0x7F); // 25.0625 C
    set_temperature(12, -162, 0x7F); // -10.125 C
    run_iteration();
    sc_sensors_snapshot(&result);
    assert(wait_ms >= 750 && result.temp1_valid && result.temp2_valid);
    assert(result.temp1_c == 25.0625f && result.temp2_c == -10.125f);
    assert(gpio_mask == ((1U << 14) | (1U << 12)));
    assert(publications == 2 && !strcmp(payload[0], "25.06"));
    assert(strtof(payload[1], NULL) < -10.11f && strtof(payload[1], NULL) > -10.14f);
    httpd_req_t response = {0};
    assert(sensors(&response) == ESP_OK);
    assert(strstr(response.body, "\"ATEMP1\":25.06"));
    assert(strstr(response.body, "\"ATEMP2\":-10.1"));
    config.temp_control.low_c = 40.1f;
    config.temp_control.high_c = 90.2f;
    config.temp_control.buffer_c = 8.3f;
    strcpy(config.temp_topic1, "quote\"slash\\line\n");
    memset(&response, 0, sizeof(response));
    assert(temperature_config(&response) == ESP_OK && response.finished);
    assert(strstr(response.body, "\"tank\":25.0625"));
    assert(strstr(response.body, "\"panel\":-10.1250"));
    assert(strstr(response.body, "\"low\":40.1000"));
    assert(strstr(response.body, "quote\\\"slash\\\\line\\u000a"));
    memset(config.temp_topic1, 1, SC_TOPIC_LEN - 1);
    config.temp_topic1[SC_TOPIC_LEN - 1] = 0;
    memcpy(config.temp_topic2, config.temp_topic1, SC_TOPIC_LEN);
    memset(&response, 0, sizeof(response));
    assert(temperature_config(&response) == ESP_OK && response.used > 1000);
    memset(&response, 0, sizeof(response)); response.fail_send = true;
    assert(temperature_config(&response) == ESP_FAIL && !response.finished);
    strcpy(config.temp_topic1, "tank"); strcpy(config.temp_topic2, "panel");
    char formatted[32];
    assert(sc_format_fixed(formatted, sizeof(formatted), 0, 2) && !strcmp(formatted, "0.00"));
    assert(sc_format_fixed(formatted, sizeof(formatted), -55, 4) && !strcmp(formatted, "-55.0000"));
    assert(!sc_format_fixed(formatted, 2, 25, 2));
    assert(!sc_format_fixed(formatted, sizeof(formatted), NAN, 2));
    assert(!sc_format_fixed(formatted, sizeof(formatted), INFINITY, 2));
    assert(!sc_format_fixed(formatted, sizeof(formatted), 1e10f, 2));

    for (int bit = 0; bit < 72; bit++) {
        set_temperature(14, 401, 0x7F);
        scratch[14][bit / 8] ^= 1U << (bit % 8);
        value = -999;
        assert(!ds_read(14, &value) && value == -999);
    }
    memset(scratch[14], 0, 9);
    assert(!ds_read(14, &value));
    memset(scratch[14], 0xFF, 9);
    assert(!ds_read(14, &value));
    set_temperature(14, -880, 0x7F);
    assert(ds_read(14, &value) && value == -55.0f);
    set_temperature(14, 2000, 0x7F);
    assert(ds_read(14, &value) && value == 125.0f);
    set_temperature(14, 2001, 0x7F);
    assert(!ds_read(14, &value));
    set_temperature(14, -881, 0x7F);
    assert(!ds_read(14, &value));
    set_temperature(14, 407, 0x1F);
    assert(ds_read(14, &value) && value == 25.0f);
    complete[14] = false;
    assert(!ds_read(14, &value));
    complete[14] = true; present[14] = false;
    assert(!ds_start(14) && !ds_read(14, &value));
    assert(!ds_start(6) && !ds_start(16));
    gpio_result = ESP_FAIL;
    assert(!ds_start(13));
    gpio_result = ESP_OK;

    config.temp_control = (sc_temp_control_config_t){.enabled=1, .relay=0,
                                                    .low_c=40, .high_c=90, .buffer_c=8};
    set_temperature(14, 400, 0x7F); set_temperature(12, 960, 0x7F);
    run_iteration();
    assert(relay_calls == 1);
    scratch[12][8] ^= 1;
    run_iteration();
    assert(snap.temp1_valid && !snap.temp2_valid && publications == 1 && relay_calls == 0);
    assert(sensors(&response) == ESP_OK);
    assert(strstr(response.body, "\"ATEMP2\":\"-\""));
    memset(&response, 0, sizeof(response));
    assert(temperature_config(&response) == ESP_OK && strstr(response.body, "\"panel\":null"));
    present[14] = false;
    run_iteration();
    assert(!snap.temp1_valid && publications == 0);
    set_temperature(14, 400, 0x7F); set_temperature(12, 960, 0x7F);
    run_iteration();
    assert(snap.temp1_valid && snap.temp2_valid && publications == 2);
    config.ds18b20_active = 0;
    run_iteration();
    assert(!snap.temp1_valid && !snap.temp2_valid && publications == 0);
    config.ds18b20_active = 1; config.ds18b20_second_gpio = 14;
    run_iteration();
    assert(snap.temp1_valid && !snap.temp2_valid && publications == 1);

    now_us += 1500000;
    sc_sensors_poll();
    assert(converting);
    config.ds18b20_active = 0;
    sc_sensors_poll();
    assert(!converting && !snap.temp1_valid && !snap.temp2_valid);
    config.ds18b20_active = 1;
    sc_sensors_poll();
    assert(converting);
    config.ds18b20_gpio = 13;
    present[13] = false;
    sc_sensors_poll();
    assert(converting && !started1 && started2); // Do not read the old GPIO.
    assert(sc_sensors_ensure_started() == ESP_OK);
    puts("PASS: late enable, allocation-free polling/cancellation, GPIO setup/conflicts, CRC corruption, "
         "signed temperatures, limits/resolution, conversion wait, snapshots/MQTT/web JSON, "
         "disconnect/recovery, disable and invalid-data relay suppression");
    return 0;
}
'''


def main():
    sensor = (PROJECT_DIR / "components/sc_sensors/sc_sensors.c").read_text(encoding="utf-8")
    timing = sensor[sensor.index("static void IRAM_ATTR delay_us("):sensor.index("static bool ds_start(")]
    logic = sensor[sensor.index("typedef struct {"):sensor.index("static void IRAM_ATTR delay_us(")]
    logic += sensor[sensor.index("static bool ds_start("):sensor.index("static float acs_read_rms(")]
    logic += sensor[sensor.index("static int temperature_control("):]
    web = (PROJECT_DIR / "components/sc_web/sc_web.c").read_text(encoding="utf-8")
    web_logic = web[web.index("static esp_err_t sensors("):web.index("static esp_err_t feature_flags(")]
    web_logic += web[web.index("typedef struct {\n  httpd_req_t *req;"):web.index("static void json_escape(")]
    web_logic += web[web.index("static esp_err_t temperature_config("):web.index("static esp_err_t apply_temperature(")]
    headers = {
        "sc_types.h": "components/sc_core/include/sc_types.h",
        "sc_config.h": "components/sc_config/include/sc_config.h",
        "sc_sensors.h": "components/sc_sensors/include/sc_sensors.h",
        "sc_relay.h": "components/sc_relay/include/sc_relay.h",
        "sc_health.h": "components/sc_health/include/sc_health.h",
        "sc_format.h": "components/sc_core/include/sc_format.h",
    }
    with tempfile.TemporaryDirectory(prefix="ds18b20-") as temp:
        work = Path(temp)
        for name, path in headers.items():
            (work / name).write_text((PROJECT_DIR / path).read_text(encoding="utf-8"), encoding="utf-8")
        (work / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 0x101\n", encoding="utf-8")
        (work / "test.c").write_text(MOCKS + logic + WEB_MOCKS + web_logic + CHECKS, encoding="utf-8")
        prefix = ["wsl", "--cd", str(work), "--exec"] if os.name == "nt" else []
        subprocess.run(prefix + ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                                 "test.c", "-o", "test"], cwd=work, check=True)
        subprocess.run(prefix + ["./test"], cwd=work, check=True)
        (work / "timing.c").write_text(TIMING_MOCKS + timing + TIMING_CHECKS, encoding="utf-8")
        subprocess.run(prefix + ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                                 "timing.c", "-o", "timing"], cwd=work, check=True)
        subprocess.run(prefix + ["./timing"], cwd=work, check=True)


if __name__ == "__main__":
    main()
