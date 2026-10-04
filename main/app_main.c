#include "esp_log.h"
#include "nvs_flash.h"
#include "sc_config.h"
#include "sc_health.h"
#include "sc_status_led.h"
#include "sc_relay.h"
#include "sc_wifi.h"
#include "sc_mqtt.h"
#include "sc_inputs.h"
#include "sc_webfs.h"
#include "sc_web.h"
#include "sc_time.h"
#include "sc_timer.h"
#include "sc_sensors.h"
#include "sc_modbus.h"
#include "sc_ha.h"
#include "sc_diag.h"
#include "sc_automation.h"
static const char *TAG="smartconfig";
void app_main(void){
 esp_err_t err=nvs_flash_init();
 if(err==ESP_ERR_NVS_NO_FREE_PAGES||err==ESP_ERR_NVS_NEW_VERSION_FOUND){ESP_LOGW(TAG,"NVS requires recovery (%d), erasing",err);ESP_ERROR_CHECK(nvs_flash_erase());err=nvs_flash_init();}
 ESP_ERROR_CHECK(err);
 ESP_ERROR_CHECK(sc_config_init());
 ESP_ERROR_CHECK(sc_health_init());
 ESP_ERROR_CHECK(sc_diag_init());
 ESP_ERROR_CHECK(sc_webfs_init());
 ESP_ERROR_CHECK(sc_config_storage_ready());
 ESP_ERROR_CHECK(sc_relay_init());
 ESP_ERROR_CHECK(sc_status_led_init());
 ESP_ERROR_CHECK(sc_wifi_init());
 ESP_ERROR_CHECK(sc_time_init());
 ESP_ERROR_CHECK(sc_automation_init());
 ESP_ERROR_CHECK(sc_mqtt_init());
 ESP_ERROR_CHECK(sc_inputs_init());
 ESP_ERROR_CHECK(sc_sensors_init());
 ESP_ERROR_CHECK(sc_timer_init());
 ESP_ERROR_CHECK(sc_modbus_init());
 ESP_ERROR_CHECK(sc_ha_init());
 ESP_ERROR_CHECK(sc_web_init());
 ESP_LOGI(TAG,"SmartConfig-AI RTOS runtime started");
}
