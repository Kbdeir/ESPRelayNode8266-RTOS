#include "esp_ota_ops.h"
#include "sc_ota.h"
#define ESP8266_APP_WINDOW_SIZE 0x100000u
static esp_ota_handle_t h;static const esp_partition_t*p;static int active;
esp_err_t sc_ota_begin(size_t size){if(active)return ESP_ERR_INVALID_STATE;p=esp_ota_get_next_update_partition(NULL);const esp_partition_t*running=esp_ota_get_running_partition();if(!p||!running)return ESP_ERR_NOT_FOUND;uint32_t window_offset=p->address&(ESP8266_APP_WINDOW_SIZE-1);if((running->address&(ESP8266_APP_WINDOW_SIZE-1))!=window_offset||window_offset+p->size>ESP8266_APP_WINDOW_SIZE||size>p->size)return ESP_ERR_INVALID_SIZE;esp_err_t e=esp_ota_begin(p,size?size:OTA_SIZE_UNKNOWN,&h);if(e==ESP_OK)active=1;return e;}
esp_err_t sc_ota_write(const void*d,size_t n){return active?esp_ota_write(h,d,n):ESP_ERR_INVALID_STATE;}
esp_err_t sc_ota_finish(void){if(!active)return ESP_ERR_INVALID_STATE;esp_err_t e=esp_ota_end(h);active=0;if(e==ESP_OK)e=esp_ota_set_boot_partition(p);return e;}
void sc_ota_abort(void){if(active){esp_ota_end(h);active=0;}}
