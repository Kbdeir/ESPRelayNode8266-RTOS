#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "sc_config.h"
#include "sc_mqtt.h"
#include "sc_relay.h"
#include "sc_sensors.h"

typedef struct {
    uint8_t ds_active, gpio1, gpio2, relay_count;
    sc_temp_control_config_t temp_control;
    struct {
        uint8_t enabled, model;
        uint16_t zero;
    } relay[SC_MAX_RELAYS];
} sensor_config_t;

static SemaphoreHandle_t mux;
static sc_sensor_snapshot_t snap;
static sensor_config_t sensor_cfg;

static void load_config(void)
{
    const sc_config_t *cfg = sc_config_acquire();
    sensor_cfg.ds_active = cfg->ds18b20_active;
    sensor_cfg.gpio1 = cfg->ds18b20_gpio;
    sensor_cfg.gpio2 = cfg->ds18b20_second_gpio;
    sensor_cfg.relay_count = cfg->relay_count <= SC_MAX_RELAYS ? cfg->relay_count : SC_MAX_RELAYS;
    sensor_cfg.temp_control = cfg->temp_control;
    for (int i = 0; i < sensor_cfg.relay_count; i++) {
        sensor_cfg.relay[i].enabled = cfg->relay[i].acs_enabled;
        sensor_cfg.relay[i].model = cfg->relay[i].acs_model_amps;
        sensor_cfg.relay[i].zero = cfg->relay[i].acs_zero;
    }
    sc_config_release();
}

static void copy_sensor_topic(char *out, size_t size, int relay)
{
    const sc_config_t *cfg = sc_config_acquire();
    const char *topic = relay < 0 ? cfg->temp_topic1
                        : relay == SC_MAX_RELAYS ? cfg->temp_topic2
                        : cfg->relay[relay].acs_topic;
    snprintf(out, size, "%s", topic);
    sc_config_release();
}

static void delay_us(uint32_t us)
{
    int64_t end = esp_timer_get_time() + us;
    while (esp_timer_get_time() < end) {;}
}

static void ow_out(uint8_t pin, int level)
{
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
}

static void ow_in(uint8_t pin)
{
    gpio_set_direction(pin, GPIO_MODE_INPUT);
}

static int ow_reset(uint8_t pin)
{
    taskENTER_CRITICAL();
    ow_out(pin, 0);
    delay_us(480);
    ow_in(pin);
    delay_us(70);
    int present = !gpio_get_level(pin);
    delay_us(410);
    taskEXIT_CRITICAL();
    return present;
}

static void ow_write_bit(uint8_t pin, int bit)
{
    taskENTER_CRITICAL();
    ow_out(pin, 0);
    if (bit) {
        delay_us(6);
        ow_in(pin);
        delay_us(64);
    } else {
        delay_us(60);
        ow_in(pin);
        delay_us(10);
    }
    taskEXIT_CRITICAL();
}

static int ow_read_bit(uint8_t pin)
{
    taskENTER_CRITICAL();
    ow_out(pin, 0);
    delay_us(6);
    ow_in(pin);
    delay_us(9);
    int bit = gpio_get_level(pin);
    delay_us(55);
    taskEXIT_CRITICAL();
    return bit;
}

static void ow_write(uint8_t pin, uint8_t value)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(pin, value & 1);
        value >>= 1;
    }
}

static uint8_t ow_read(uint8_t pin)
{
    uint8_t value = 0;
    for (int i = 0; i < 8; i++) value |= ow_read_bit(pin) << i;
    return value;
}

static bool ds_start(uint8_t pin)
{
    if (!ow_reset(pin)) return false;
    ow_write(pin, 0xCC);
    ow_write(pin, 0x44);
    return true;
}

static bool ds_read(uint8_t pin, float *out)
{
    if (!ow_reset(pin)) return false;
    ow_write(pin, 0xCC);
    ow_write(pin, 0xBE);
    uint8_t low = ow_read(pin), high = ow_read(pin);
    int16_t raw = (int16_t)((high << 8) | low);
    *out = raw / 16.0f;
    return *out > -55 && *out < 125;
}

static float acs_read_rms(uint8_t model, uint16_t zero)
{
    const float sensitivity = model == 5 ? 0.185f : model == 20 ? 0.100f : 0.066f;
    double sum = 0;
    int samples = 64;
    for (int i = 0; i < samples; i++) {
        uint16_t adc = 0;
        if (adc_read(&adc) != ESP_OK) continue;
        float volts = ((int)adc - (int)zero) * (5.0f / 1023.0f);
        float amps = volts / sensitivity;
        sum += amps * amps;
        delay_us(300);
    }
    return sqrtf((float)(sum / samples));
}

static int temperature_control(float tank_c, float panel_c)
{
    const sc_temp_control_config_t *control = &sensor_cfg.temp_control;
    if (!control->enabled || control->relay >= sensor_cfg.relay_count ||
        control->low_c == 0.0f || control->buffer_c == 0.0f) return -1;

    bool desired;
    if (panel_c < control->low_c ||
        (control->high_c != 0.0f && panel_c > control->high_c)) {
        desired = false;
    } else {
        float difference = panel_c - tank_c;
        if (difference > control->buffer_c) desired = true;
        else if (difference < control->buffer_c) desired = false;
        else return -1;
    }

    sc_relay_cmd_t command = {
        .relay = control->relay,
        .op = SC_RELAY_SET,
        .value = desired,
    };
    return sc_relay_submit(&command) ? (desired ? 1 : 0) : -1;
}

static void task(void *arg)
{
    (void)arg;
    adc_config_t adc_cfg = {.mode = ADC_READ_TOUT_MODE, .clk_div = 8};
    adc_init(&adc_cfg);
    for (;;) {
        load_config();
        float temp1 = 0, temp2 = 0, amps = 0;
        bool temp1_valid = false, temp2_valid = false, amps_valid = false;
        if (sensor_cfg.ds_active) {
            bool started1 = ds_start(sensor_cfg.gpio1);
            bool started2 = sensor_cfg.gpio2 != sensor_cfg.gpio1 ? ds_start(sensor_cfg.gpio2) : false;
            vTaskDelay(pdMS_TO_TICKS(760));
            if (started1) temp1_valid = ds_read(sensor_cfg.gpio1, &temp1);
            if (started2) temp2_valid = ds_read(sensor_cfg.gpio2, &temp2);
        }
        int control_call = -1;
        if (temp1_valid && temp2_valid)
            control_call = temperature_control(temp1, temp2);
        for (int i = 0; i < sensor_cfg.relay_count; i++) {
            if (!sensor_cfg.relay[i].enabled) continue;
            uint16_t zero = sensor_cfg.relay[i].zero ? sensor_cfg.relay[i].zero : 512;
            amps = acs_read_rms(sensor_cfg.relay[i].model, zero);
            amps_valid = true;
            char value[24];
            char topic[SC_TOPIC_LEN];
            snprintf(value, sizeof(value), "%.2f", amps);
            copy_sensor_topic(topic, sizeof(topic), i);
            sc_mqtt_publish_qos0(topic, value, true);
            break;
        }

        xSemaphoreTake(mux, portMAX_DELAY);
        snap.temp1_valid = temp1_valid;
        snap.temp2_valid = temp2_valid;
        snap.acs_valid = amps_valid;
        snap.temp1_c = temp1;
        snap.temp2_c = temp2;
        snap.acs_amps = amps;
        snap.temp_control_enabled = sensor_cfg.temp_control.enabled;
        if (control_call >= 0) snap.temp_control_call = control_call != 0;
        snap.temp_delta_c = temp2_valid && temp1_valid ? temp2 - temp1 : 0.0f;
        snap.updated_us = esp_timer_get_time();
        xSemaphoreGive(mux);

        if (temp1_valid) {
            char value[24];
            char topic[SC_TOPIC_LEN];
            snprintf(value, sizeof(value), "%.2f", temp1);
            copy_sensor_topic(topic, sizeof(topic), -1);
            sc_mqtt_publish_qos0(topic, value, true);
        }
        if (temp2_valid) {
            char value[24];
            char topic[SC_TOPIC_LEN];
            snprintf(value, sizeof(value), "%.2f", temp2);
            copy_sensor_topic(topic, sizeof(topic), SC_MAX_RELAYS);
            sc_mqtt_publish_qos0(topic, value, true);
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

esp_err_t sc_sensors_init(void)
{
    bool needed;
    const sc_config_t *cfg = sc_config_acquire();
    needed = cfg->ds18b20_active || cfg->temp_control.enabled;
    for (uint8_t i = 0; !needed && i < cfg->relay_count && i < SC_MAX_RELAYS; i++)
        needed = cfg->relay[i].acs_enabled;
    sc_config_release();
    if (!needed) return ESP_OK;
    mux = xSemaphoreCreateMutex();
    if (!mux) return ESP_ERR_NO_MEM;
    return xTaskCreate(task, "sensors", 2304, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void sc_sensors_snapshot(sc_sensor_snapshot_t *out)
{
    if (!out) return;
    if (!mux) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(mux, portMAX_DELAY);
    *out = snap;
    xSemaphoreGive(mux);
}
