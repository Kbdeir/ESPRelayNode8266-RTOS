#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sc_status_led.h"

#define STATUS_LED_GPIO GPIO_NUM_16
#define BLINK_CONNECTING_MS 50U
#define BLINK_AP_MODE_MS 100U
#define BLINK_CONNECTED_MS 1000U

static const char *TAG = "status_led";
static TimerHandle_t blink_timer;
static sc_status_led_mode_t current_mode;
static bool mode_set;
static bool led_on;

static uint32_t interval_ms(sc_status_led_mode_t mode)
{
    switch (mode) {
        case SC_STATUS_LED_CONNECTED:
            return BLINK_CONNECTED_MS;
        case SC_STATUS_LED_AP_MODE:
            return BLINK_AP_MODE_MS;
        case SC_STATUS_LED_CONNECTING:
        default:
            return BLINK_CONNECTING_MS;
    }
}

static void write_led(bool on)
{
    led_on = on;
    gpio_set_level(STATUS_LED_GPIO, on ? 0 : 1);
}

static void blink_cb(TimerHandle_t timer)
{
    (void)timer;
    write_led(!led_on);
}

esp_err_t sc_status_led_init(void)
{
    gpio_config_t gpio = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&gpio);
    if (err != ESP_OK) return err;

    write_led(false);
    blink_timer = xTimerCreate("status_led", pdMS_TO_TICKS(BLINK_CONNECTED_MS),
                               pdTRUE, NULL, blink_cb);
    return blink_timer ? ESP_OK : ESP_ERR_NO_MEM;
}

void sc_status_led_set_mode(sc_status_led_mode_t mode)
{
    if (!blink_timer || (mode_set && current_mode == mode)) return;

    current_mode = mode;
    mode_set = true;
    write_led(false);
    if (xTimerChangePeriod(blink_timer, pdMS_TO_TICKS(interval_ms(mode)), 0) != pdPASS)
        ESP_LOGW(TAG, "failed to change blink interval");
}
