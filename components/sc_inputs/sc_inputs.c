#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sc_config.h"
#include "sc_inputs.h"
#include "sc_mqtt.h"
#include "sc_relay.h"

#define INPUT_DEBOUNCE_MS 25

typedef struct {
    sc_input_config_t config;
    bool configured;
    bool raw;
    bool stable;
    TickType_t changed_at;
} input_runtime_t;

static input_runtime_t inputs[SC_MAX_INPUTS];
static uint8_t input_count;
static TickType_t mqtt_checked_at;
static bool mqtt_was_connected;
static const char *TAG = "inputs";

static bool conflicts_with_onewire(bool active, uint8_t first_gpio,
                                   uint8_t second_gpio, uint8_t gpio)
{
    return active && (gpio == first_gpio || gpio == second_gpio);
}

static void publish_input(const input_runtime_t *input, const char *payload)
{
    if (input->config.topic[0])
        sc_mqtt_publish(input->config.topic, payload, true);
}

static void relay_command(const input_runtime_t *input, sc_relay_op_t op, bool value)
{
    if (input->config.relay < 0 || input->config.relay >= sc_relay_count()) return;
    sc_relay_cmd_t command = {
        .relay = (uint8_t)input->config.relay,
        .op = op,
        .value = value,
    };
    if (!sc_relay_submit(&command))
        ESP_LOGW(TAG, "relay queue full for GPIO %u", (unsigned)input->config.gpio);
}

static void input_changed(input_runtime_t *input, bool level)
{
    switch ((sc_input_mode_t)input->config.mode) {
    case SC_INPUT_MQTT_TOGGLE:
        if (!level) publish_input(input, "tog");
        break;
    case SC_INPUT_MQTT_STATE:
        publish_input(input, level ? "on" : "off");
        break;
    case SC_INPUT_RELAY_TOGGLE:
        if (!level) {
            relay_command(input, SC_RELAY_TOGGLE, false);
            publish_input(input, "tog");
        }
        break;
    case SC_INPUT_COPY_TO_RELAY:
        relay_command(input, SC_RELAY_SET, level);
        publish_input(input, level ? "on" : "off");
        break;
    default:
        break;
    }
}

void sc_inputs_poll(void)
{
    const TickType_t debounce = pdMS_TO_TICKS(INPUT_DEBOUNCE_MS);
    TickType_t now = xTaskGetTickCount();
    for (uint8_t i = 0; i < input_count; i++) {
        input_runtime_t *input = &inputs[i];
        if (!input->configured) continue;
        bool level = gpio_get_level((gpio_num_t)input->config.gpio) != 0;
        if (level != input->raw) {
            input->raw = level;
            input->changed_at = now;
        } else if (level != input->stable && now - input->changed_at >= debounce) {
            input->stable = level;
            input_changed(input, level);
        }
    }
    if (now - mqtt_checked_at >= pdMS_TO_TICKS(1000)) {
        sc_mqtt_status_t mqtt;
        mqtt_checked_at = now;
        sc_mqtt_status(&mqtt);
        if (mqtt.connected && !mqtt_was_connected) {
            for (uint8_t i = 0; i < input_count; i++) {
                input_runtime_t *input = &inputs[i];
                if (input->configured &&
                    (input->config.mode == SC_INPUT_MQTT_STATE ||
                     input->config.mode == SC_INPUT_COPY_TO_RELAY))
                    publish_input(input, input->stable ? "on" : "off");
            }
        }
        mqtt_was_connected = mqtt.connected;
    }
}

esp_err_t sc_inputs_init(void)
{
    const sc_config_t *cfg = sc_config_acquire();
    input_count = cfg->input_count <= SC_MAX_INPUTS ? cfg->input_count : SC_MAX_INPUTS;
    uint8_t relay_count = cfg->relay_count;
    bool onewire_active = cfg->ds18b20_active;
    uint8_t onewire_gpio = cfg->ds18b20_gpio;
    uint8_t onewire_second_gpio = cfg->ds18b20_second_gpio;
    memset(inputs, 0, sizeof(inputs));
    for (uint8_t i = 0; i < input_count; i++)
        inputs[i].config = cfg->input[i];
    sc_config_release();

    for (uint8_t i = 0; i < input_count; i++) {
        input_runtime_t *input = &inputs[i];
        if (input->config.mode == SC_INPUT_NONE ||
            input->config.mode == SC_INPUT_TEMPERATURE) continue;
        if (input->config.mode > SC_INPUT_COPY_TO_RELAY) {
            ESP_LOGW(TAG, "input %u has invalid mode %u", (unsigned)i,
                     (unsigned)input->config.mode);
            continue;
        }
        if (conflicts_with_onewire(onewire_active, onewire_gpio,
                                   onewire_second_gpio, input->config.gpio)) {
            ESP_LOGW(TAG, "GPIO %u is reserved by DS18B20; input %u disabled",
                     (unsigned)input->config.gpio, (unsigned)i);
            continue;
        }
        if ((input->config.mode == SC_INPUT_RELAY_TOGGLE ||
            input->config.mode == SC_INPUT_COPY_TO_RELAY) &&
            (input->config.relay < 0 || input->config.relay >= relay_count)) {
            ESP_LOGW(TAG, "input %u has no valid relay mapping", (unsigned)i);
            continue;
        }

        gpio_config_t gpio = {
            .pin_bit_mask = 1ULL << input->config.gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&gpio);
        if (err != ESP_OK) return err;
        input->raw = gpio_get_level((gpio_num_t)input->config.gpio) != 0;
        input->stable = input->raw;
        input->changed_at = xTaskGetTickCount();
        input->configured = true;
        ESP_LOGI(TAG, "input %u GPIO %u mode %u relay %d", (unsigned)i,
                 (unsigned)input->config.gpio, (unsigned)input->config.mode,
                 (int)input->config.relay);
    }

    mqtt_checked_at = xTaskGetTickCount();
    mqtt_was_connected = false;
    return ESP_OK;
}

void sc_inputs_snapshot(sc_input_status_t out[SC_MAX_INPUTS])
{
    if (!out) return;
    taskENTER_CRITICAL();
    for (uint8_t i = 0; i < SC_MAX_INPUTS; i++) {
        out[i].configured = inputs[i].configured;
        out[i].level = inputs[i].stable;
        out[i].gpio = inputs[i].config.gpio;
        out[i].mode = inputs[i].config.mode;
        out[i].relay = inputs[i].config.relay;
    }
    taskEXIT_CRITICAL();
}
