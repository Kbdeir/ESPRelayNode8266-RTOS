#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/soc.h"
#include "esp8266/gpio_struct.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "sc_config.h"
#include "sc_mqtt.h"
#include "sc_relay.h"
#include "sc_sensors.h"
#include "sc_format.h"

typedef struct {
    uint8_t ds_active, gpio1, gpio2, relay_count;
    sc_temp_control_config_t temp_control;
    struct {
        uint8_t enabled, model;
        uint16_t zero;
    } relay[SC_MAX_RELAYS];
} sensor_config_t;

static sc_sensor_snapshot_t snap;
static sensor_config_t sensor_cfg;
static bool initialized, adc_initialized, converting, started1, started2;
static int64_t conversion_ready_us, next_sample_us;
static uint32_t prepared_pins;

static bool ds_gpio_valid(uint8_t pin)
{
    return pin == 0 || pin == 2 || pin == 4 || pin == 5 ||
           (pin >= 12 && pin <= 15);
}

static bool ds_bus_config_valid(const sc_config_t *cfg)
{
    if (!ds_gpio_valid(cfg->ds18b20_gpio) ||
        !ds_gpio_valid(cfg->ds18b20_second_gpio)) return false;
    for (uint8_t i = 0; i < cfg->relay_count && i < SC_MAX_RELAYS; i++)
        if (cfg->relay[i].gpio == cfg->ds18b20_gpio ||
            cfg->relay[i].gpio == cfg->ds18b20_second_gpio) return false;
    return true;
}

bool sc_sensors_validate_config(const sc_config_t *cfg)
{
    if (!cfg || !cfg->ds18b20_active) return cfg != NULL;
    if (!ds_bus_config_valid(cfg)) return false;
    for (uint8_t i = 0; i < cfg->input_count && i < SC_MAX_INPUTS; i++)
        if (cfg->input[i].mode > SC_INPUT_NONE &&
            cfg->input[i].mode < SC_INPUT_TEMPERATURE &&
            (cfg->input[i].gpio == cfg->ds18b20_gpio ||
             cfg->input[i].gpio == cfg->ds18b20_second_gpio)) return false;
    return true;
}

static void load_config(void)
{
    const sc_config_t *cfg = sc_config_acquire();
    // Digital inputs already yield these GPIOs to DS18B20, including old saved mappings.
    sensor_cfg.ds_active = cfg->ds18b20_active && ds_bus_config_valid(cfg);
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

static void IRAM_ATTR delay_us(uint32_t us)
{
    uint32_t start = soc_get_ccount();
    uint32_t ticks = us * g_esp_ticks_per_us;
    while ((uint32_t)(soc_get_ccount() - start) < ticks) {;}
}

static inline __attribute__((always_inline)) void ow_low(uint8_t pin)
{
    uint32_t mask = 1U << pin;
    GPIO.out_w1tc = mask;
    GPIO.enable_w1ts = mask;
}

static inline __attribute__((always_inline)) void ow_in(uint8_t pin)
{
    GPIO.enable_w1tc = 1U << pin;
}

static inline __attribute__((always_inline)) int ow_level(uint8_t pin)
{
    return (GPIO.in >> pin) & 1U;
}

// Keep the entire timed slot in IRAM: a flash cache miss can exceed the read window.
static int IRAM_ATTR ow_reset(uint8_t pin)
{
    taskENTER_CRITICAL();
    ow_low(pin);
    delay_us(480);
    ow_in(pin);
    delay_us(70);
    int present = !ow_level(pin);
    delay_us(410);
    present = present && ow_level(pin);
    taskEXIT_CRITICAL();
    return present;
}

static void IRAM_ATTR __attribute__((noinline)) ow_write_bit(uint8_t pin, int bit)
{
    taskENTER_CRITICAL();
    ow_low(pin);
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

static int IRAM_ATTR ow_read_bit(uint8_t pin)
{
    taskENTER_CRITICAL();
    ow_low(pin);
    delay_us(3);
    ow_in(pin);
    delay_us(9);
    int bit = ow_level(pin);
    delay_us(58);
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
    if (!ds_gpio_valid(pin)) return false;
    if (!(prepared_pins & (1U << pin))) {
        gpio_config_t gpio = {
            .pin_bit_mask = 1ULL << pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&gpio) != ESP_OK) return false;
        prepared_pins |= 1U << pin;
    }
    if (!ow_reset(pin)) return false;
    ow_write(pin, 0xCC);
    ow_write(pin, 0x44);
    return true;
}

static bool ds_read(uint8_t pin, float *out)
{
    if (!out || !ow_read_bit(pin) || !ow_reset(pin)) return false;
    ow_write(pin, 0xCC);
    ow_write(pin, 0xBE);
    uint8_t scratch[9], crc = 0, nonzero = 0;
    for (size_t i = 0; i < sizeof(scratch); i++) {
        scratch[i] = ow_read(pin);
        nonzero |= scratch[i];
        uint8_t value = scratch[i];
        for (int bit = 0; bit < 8; bit++) {
            bool mix = (crc ^ value) & 1U;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            value >>= 1;
        }
    }
    if (crc || !nonzero) return false;
    int16_t raw = (int16_t)(((uint16_t)scratch[1] << 8) | scratch[0]);
    uint8_t resolution = (scratch[4] >> 5) & 3U;
    raw &= ~((1U << (3U - resolution)) - 1U);
    float value = raw / 16.0f;
    if (value < -55.0f || value > 125.0f) return false;
    *out = value;
    return true;
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

void sc_sensors_poll(void) {
    if (!initialized)
        return;
    bool previous_active = sensor_cfg.ds_active;
    uint8_t previous_gpio1 = sensor_cfg.gpio1,
            previous_gpio2 = sensor_cfg.gpio2;
    load_config();
    int64_t now = esp_timer_get_time();
    if (converting && (!sensor_cfg.ds_active || !previous_active ||
                       previous_gpio1 != sensor_cfg.gpio1 ||
                       previous_gpio2 != sensor_cfg.gpio2)) {
        converting = false;
        next_sample_us = 0;
    }
    float temp1 = 0, temp2 = 0, amps = 0;
    bool temp1_valid = false, temp2_valid = false, amps_valid = false;
    if (converting) {
        if (now < conversion_ready_us)
            return;
        converting = false;
        if (started1)
            temp1_valid = ds_read(sensor_cfg.gpio1, &temp1);
        if (started2)
            temp2_valid = ds_read(sensor_cfg.gpio2, &temp2);
    } else {
        if (now < next_sample_us && previous_active == sensor_cfg.ds_active &&
            previous_gpio1 == sensor_cfg.gpio1 &&
            previous_gpio2 == sensor_cfg.gpio2)
            return;
        if (sensor_cfg.ds_active) {
            started1 = ds_start(sensor_cfg.gpio1);
            started2 = sensor_cfg.gpio2 != sensor_cfg.gpio1
                           ? ds_start(sensor_cfg.gpio2)
                           : false;
            if (started1 || started2) {
                converting = true;
                conversion_ready_us = esp_timer_get_time() + 760000;
                return;
            }
        }
    }
    int control_call = -1;
    if (temp1_valid && temp2_valid)
        control_call = temperature_control(temp1, temp2);
    for (int i = 0; i < sensor_cfg.relay_count; i++) {
        if (!sensor_cfg.relay[i].enabled)
            continue;
        if (!adc_initialized) {
            adc_config_t adc_cfg = {.mode = ADC_READ_TOUT_MODE, .clk_div = 8};
            adc_init(&adc_cfg);
            adc_initialized = true;
        }
        uint16_t zero =
            sensor_cfg.relay[i].zero ? sensor_cfg.relay[i].zero : 512;
        amps = acs_read_rms(sensor_cfg.relay[i].model, zero);
        amps_valid = true;
        char value[24];
        char topic[SC_TOPIC_LEN];
        if (!sc_format_fixed(value, sizeof(value), amps, 2))
            continue;
        copy_sensor_topic(topic, sizeof(topic), i);
        sc_mqtt_publish_qos0(topic, value, true);
        break;
    }

    taskENTER_CRITICAL();
    snap.temp1_valid = temp1_valid;
    snap.temp2_valid = temp2_valid;
    snap.acs_valid = amps_valid;
    snap.temp1_c = temp1;
    snap.temp2_c = temp2;
    snap.acs_amps = amps;
    snap.temp_control_enabled = sensor_cfg.temp_control.enabled;
    if (control_call >= 0)
        snap.temp_control_call = control_call != 0;
    snap.temp_delta_c = temp2_valid && temp1_valid ? temp2 - temp1 : 0.0f;
    snap.updated_us = esp_timer_get_time();
    taskEXIT_CRITICAL();

    if (temp1_valid) {
        char value[24];
        char topic[SC_TOPIC_LEN];
        sc_format_fixed(value, sizeof(value), temp1, 2);
        copy_sensor_topic(topic, sizeof(topic), -1);
        sc_mqtt_publish_qos0(topic, value, true);
    }
    if (temp2_valid) {
        char value[24];
        char topic[SC_TOPIC_LEN];
        sc_format_fixed(value, sizeof(value), temp2, 2);
        copy_sensor_topic(topic, sizeof(topic), SC_MAX_RELAYS);
        sc_mqtt_publish_qos0(topic, value, true);
    }
    next_sample_us = esp_timer_get_time() + 1500000;
}

esp_err_t sc_sensors_ensure_started(void) {
    // The existing schedule task drives conversion deadlines; no sensor task or
    // mutex.
    initialized = true;
    return ESP_OK;
}

esp_err_t sc_sensors_init(void) { return sc_sensors_ensure_started(); }

void sc_sensors_snapshot(sc_sensor_snapshot_t *out)
{
    if (!out) return;
    if (!initialized) {
        memset(out, 0, sizeof(*out));
        return;
    }
    taskENTER_CRITICAL();
    *out = snap;
    taskEXIT_CRITICAL();
}
