#include <string.h>
#include "nvs.h"
#include "esp_system.h"
#include "sc_diag.h"
static sc_diag_t d;
static const char*rn(esp_reset_reason_t r){switch(r){case ESP_RST_POWERON:return "power-on";case ESP_RST_SW:return "software";case ESP_RST_PANIC:return "panic";case ESP_RST_INT_WDT:return "interrupt watchdog";case ESP_RST_TASK_WDT:return "task watchdog";case ESP_RST_WDT:return "watchdog";case ESP_RST_BROWNOUT:return "brownout";case ESP_RST_DEEPSLEEP:return "deep sleep";default:return "unknown";}}
esp_err_t sc_diag_init(void){nvs_handle h;esp_err_t e=nvs_open("diag",NVS_READWRITE,&h);if(e!=ESP_OK)return e;uint32_t boots=0;nvs_get_u32(h,"boots",&boots);boots++;nvs_set_u32(h,"boots",boots);uint32_t old=0;nvs_get_u32(h,"crash",&old);esp_reset_reason_t r=esp_reset_reason();bool crash=r==ESP_RST_PANIC||r==ESP_RST_INT_WDT||r==ESP_RST_TASK_WDT||r==ESP_RST_WDT||r==ESP_RST_BROWNOUT;if(crash){old=(uint32_t)r;nvs_set_u32(h,"crash",old);}nvs_commit(h);nvs_close(h);memset(&d,0,sizeof(d));d.boot_count=boots;d.reset_reason=old;d.has_dump=old!=0;snprintf(d.reason,sizeof(d.reason),"%s",rn((esp_reset_reason_t)old));return ESP_OK;}
void sc_diag_snapshot(sc_diag_t*out){if(out)*out=d;}
esp_err_t sc_diag_clear(void){nvs_handle h;esp_err_t e=nvs_open("diag",NVS_READWRITE,&h);if(e!=ESP_OK)return e;nvs_erase_key(h,"crash");e=nvs_commit(h);nvs_close(h);d.has_dump=false;d.reset_reason=0;snprintf(d.reason,sizeof(d.reason),"none");return e;}
