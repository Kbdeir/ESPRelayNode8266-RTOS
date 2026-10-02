#include <string.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sc_config.h"

#define CFG_VERSION 6
#define CFG_VERSION_V5 5
#define CFG_VERSION_V4 4
#define CFG_VERSION_V3 3
#define CFG_VERSION_V2 2
static const char *TAG="cfg";
static SemaphoreHandle_t lock;
static sc_config_t cfg;

static void cpy(char *dst, size_t n, const char *src){ if(!src)src=""; snprintf(dst,n,"%s",src); }
static void default_controller_id(char out[13]){
    uint8_t mac[6]={0};
    if(esp_efuse_mac_get_default(mac)!=ESP_OK){cpy(out,13,"ESP8266");return;}
    snprintf(out,13,"%02X%02X%02X%02X%02X%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

void sc_config_defaults(sc_config_t *c){
    char node_id[13];
    default_controller_id(node_id);
    memset(c,0,sizeof(*c)); c->version=CFG_VERSION;
    cpy(c->phy_loc,sizeof(c->phy_loc),"NA");
    cpy(c->ap_ssid,sizeof(c->ap_ssid),"Node-Setup");
    cpy(c->ap_password,sizeof(c->ap_password),"12345678");
    cpy(c->timeserver,sizeof(c->timeserver),"pool.ntp.org"); c->timezone_hours=0;
    c->mqtt_port=1883; c->mqtt_keepalive_sec=60; c->ha_discovery_active=0;
    c->modbus_active=1; c->modbus_port=502; c->modbus_unit_id=1;
    cpy(c->web_user,sizeof(c->web_user),"user");
    cpy(c->web_password,sizeof(c->web_password),"pass");
    c->ds18b20_active=0; c->ds18b20_gpio=14; c->ds18b20_second_gpio=12;
    snprintf(c->temp_topic1,SC_TOPIC_LEN,"/home/Controller%s/Temp/1",node_id);
    snprintf(c->temp_topic2,SC_TOPIC_LEN,"/home/Controller%s/Temp/2",node_id);
    c->relay_count=1;
    c->relay[0]=(sc_relay_config_t){.relay_nb=0,.gpio=5,.active_high=1,.acs_model_amps=30,.max_current=10.0f};
    c->relay[1]=(sc_relay_config_t){.relay_nb=1,.gpio=14,.active_high=1,.acs_model_amps=30,.max_current=10.0f};
    for(int i=0;i<SC_MAX_RELAYS;i++){
        snprintf(c->relay[i].pub_topic,SC_TOPIC_LEN,"/home/Controller%s/Coils/C%d",node_id,i);
        snprintf(c->relay[i].state_pub_topic,SC_TOPIC_LEN,"/home/Controller%s/Coils/State/C%d",node_id,i);
        snprintf(c->relay[i].sub_topic,SC_TOPIC_LEN,"/home/Controller%s/Coils/C%d/set",node_id,i);
        snprintf(c->relay[i].lwill_topic,SC_TOPIC_LEN,"/home/Controller%s/LWT%d",node_id,i);
        snprintf(c->relay[i].acs_topic,SC_TOPIC_LEN,"/home/Controller%s/Coils/C%d/Amps",node_id,i);
    }
    for(int i=0;i<SC_MAX_TIMERS;i++){
        cpy(c->timer_schedule[i].date_from,sizeof(c->timer_schedule[i].date_from),"1970-01-01");
        cpy(c->timer_schedule[i].date_to,sizeof(c->timer_schedule[i].date_to),"2100-01-01");
    }
    c->input_count=SC_MAX_INPUTS;
    const uint8_t input_gpio[SC_MAX_INPUTS]={13,12,14};
    for(int i=0;i<SC_MAX_INPUTS;i++){
        c->input[i].gpio=input_gpio[i];
        c->input[i].mode=SC_INPUT_NONE;
        c->input[i].relay=-1;
        snprintf(c->input[i].topic,SC_TOPIC_LEN,"/home/Controller%s/Inputs/I%d",node_id,i);
    }
    c->temp_control.enabled=0;
    c->temp_control.relay=0;
    c->temp_control.low_c=40.0f;
    c->temp_control.high_c=90.0f;
    c->temp_control.buffer_c=8.0f;
}

static esp_err_t persist_locked(void){
    nvs_handle h; esp_err_t e=nvs_open("smartcfg",NVS_READWRITE,&h); if(e!=ESP_OK)return e;
    e=nvs_set_blob(h,"config",&cfg,sizeof(cfg)); if(e==ESP_OK)e=nvs_commit(h); nvs_close(h); return e;
}

esp_err_t sc_config_init(void){
    lock=xSemaphoreCreateMutex(); if(!lock)return ESP_ERR_NO_MEM;
    sc_config_defaults(&cfg); nvs_handle h; esp_err_t e=nvs_open("smartcfg",NVS_READONLY,&h);
    if(e==ESP_OK){
        size_t n=0;e=nvs_get_blob(h,"config",NULL,&n);
        if(e==ESP_OK&&(n==sizeof(cfg)||n==offsetof(sc_config_t,web_user)||n==offsetof(sc_config_t,modbus_unit_id)||n==offsetof(sc_config_t,input_count)||n==offsetof(sc_config_t,timer_schedule))){size_t got=n;e=nvs_get_blob(h,"config",&cfg,&got);}
        nvs_close(h);
        if(e==ESP_OK&&n==sizeof(cfg)&&cfg.version==CFG_VERSION){ESP_LOGI(TAG,"loaded configuration");return ESP_OK;}
        if(e==ESP_OK&&n==offsetof(sc_config_t,web_user)&&cfg.version==CFG_VERSION_V5){
            cfg.version=CFG_VERSION;ESP_LOGI(TAG,"migrated configuration v5 to v6");return persist_locked();
        }
        if(e==ESP_OK&&n==offsetof(sc_config_t,modbus_unit_id)&&cfg.version==CFG_VERSION_V4){
            cfg.version=CFG_VERSION;ESP_LOGI(TAG,"migrated configuration v4 to v6");return persist_locked();
        }
        if(e==ESP_OK&&n==offsetof(sc_config_t,input_count)&&cfg.version==CFG_VERSION_V3){
            cfg.version=CFG_VERSION;ESP_LOGI(TAG,"migrated configuration v3 to v6");return persist_locked();
        }
        if(e==ESP_OK&&n==offsetof(sc_config_t,timer_schedule)&&cfg.version==CFG_VERSION_V2){
            for(int i=0;i<SC_MAX_TIMERS;i++){uint32_t duration=cfg.timer[i].duration_sec;cfg.timer_schedule[i].duration_hours=duration/3600;cfg.timer_schedule[i].duration_minutes=(duration%3600)/60;uint32_t end=cfg.timer[i].start_hour*60+cfg.timer[i].start_minute+duration/60;cfg.timer_schedule[i].end_hour=(end/60)%24;cfg.timer_schedule[i].end_minute=end%60;}
            cfg.version=CFG_VERSION;ESP_LOGI(TAG,"migrated configuration v2 to v6");return persist_locked();
        }
    }
    sc_config_defaults(&cfg);
    ESP_LOGW(TAG,"using defaults"); return persist_locked();
}
void sc_config_snapshot(sc_config_t *out){ xSemaphoreTake(lock,portMAX_DELAY); *out=cfg; xSemaphoreGive(lock); }
const sc_config_t *sc_config_acquire(void){ xSemaphoreTake(lock,portMAX_DELAY); return &cfg; }
void sc_config_release(void){ xSemaphoreGive(lock); }
esp_err_t sc_config_replace(const sc_config_t *in){ if(!in)return ESP_ERR_INVALID_ARG; xSemaphoreTake(lock,portMAX_DELAY); cfg=*in; cfg.version=CFG_VERSION; esp_err_t e=persist_locked(); xSemaphoreGive(lock); return e; }
