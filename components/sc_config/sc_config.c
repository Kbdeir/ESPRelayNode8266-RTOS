#include <string.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sc_config.h"
#include "sc_littlefs.h"

#define CFG_VERSION 9
#define CFG_VERSION_V8 8
#define CFG_VERSION_V7 7
#define CFG_VERSION_V6 6
#define CFG_VERSION_V5 5
#define CFG_VERSION_V4 4
#define CFG_VERSION_V3 3
#define CFG_VERSION_V2 2
static const char *TAG="cfg";
static SemaphoreHandle_t lock;
static sc_config_t cfg;
static bool storage_ready;

#define CFG_FILE "/.smartcfg"
#define CFG_TEMP_FILE "/.smartcfg.tmp"
#define CFG_FILE_MAGIC 0x53434647U

_Static_assert(sizeof(sc_remote_modbus_config_t) <= SC_REMOTE_KEY_LEN,
               "Modbus settings must fit the remote sensor metadata field");

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t hash;
} cfg_file_header_t;

static uint32_t hash_update(uint32_t hash,const void *data,size_t len){
    const uint8_t *p=data;
    while(len--){hash^=*p++;hash*=16777619U;}
    return hash;
}

static uint32_t config_hash(const sc_config_t *value){
    return hash_update(2166136261U,value,sizeof(*value));
}

static esp_err_t persist_file(const sc_config_t *value){
    if(!value||value->version!=CFG_VERSION)return ESP_ERR_INVALID_ARG;
    cfg_file_header_t header={CFG_FILE_MAGIC,CFG_VERSION,sizeof(*value),config_hash(value)};
    int result=sc_lfs_stream_open(CFG_TEMP_FILE);
    if(result<0)return ESP_FAIL;
    if(sc_lfs_stream_write(&header,sizeof(header))<0||
       sc_lfs_stream_write(value,sizeof(*value))<0)result=-1;
    if(sc_lfs_stream_close()<0)result=-1;
    if(result<0){sc_lfs_remove(CFG_TEMP_FILE);return ESP_FAIL;}
    if(sc_lfs_rename(CFG_TEMP_FILE,CFG_FILE)<0){sc_lfs_remove(CFG_TEMP_FILE);return ESP_FAIL;}
    return ESP_OK;
}

static esp_err_t load_file(void){
    cfg_file_header_t header;
    size_t n=0;
    if(sc_lfs_read_range(CFG_FILE,0,&header,sizeof(header),&n)<0||
       n!=sizeof(header)||header.magic!=CFG_FILE_MAGIC||
       !((header.version==CFG_VERSION&&header.size==sizeof(cfg))||
         (header.version==CFG_VERSION_V8&&
          header.size==offsetof(sc_config_t,modbus_rule))||
         (header.version==CFG_VERSION_V7&&
          header.size==offsetof(sc_config_t,remote_transport))))return ESP_ERR_NOT_FOUND;
    uint8_t chunk[128],extra;
    uint32_t hash=2166136261U;
    size_t offset=0;
    while(offset<header.size){
        size_t want=header.size-offset;
        if(want>sizeof(chunk))want=sizeof(chunk);
        if(sc_lfs_read_range(CFG_FILE,sizeof(header)+offset,chunk,want,&n)<0||n!=want)
            return ESP_ERR_INVALID_CRC;
        hash=hash_update(hash,chunk,n);
        offset+=n;
    }
    if(hash!=header.hash||
       sc_lfs_read_range(CFG_FILE,sizeof(header)+header.size,&extra,1,&n)<0||n)
        return ESP_ERR_INVALID_CRC;
    memset(cfg.remote_transport,0,sizeof(cfg.remote_transport));
    memset(cfg.modbus_rule,0,sizeof(cfg.modbus_rule));
    if(sc_lfs_read_range(CFG_FILE,sizeof(header),&cfg,header.size,&n)<0||
       n!=header.size||cfg.version!=header.version)return ESP_ERR_INVALID_CRC;
    if(header.version==CFG_VERSION_V7){
        cfg.version=CFG_VERSION;
        ESP_LOGI(TAG,"migrating filesystem configuration v7 to v9");
        return persist_file(&cfg);
    }
    if(header.version==CFG_VERSION_V8){
        cfg.version=CFG_VERSION;
        ESP_LOGI(TAG,"migrating filesystem configuration v8 to v9");
        return persist_file(&cfg);
    }
    return ESP_OK;
}

void sc_remote_modbus_decode(const sc_remote_sensor_config_t *sensor,
                             sc_remote_modbus_config_t *out){
    memset(out,0,sizeof(*out));
    memcpy(out,sensor->json_key,sizeof(*out));
}

void sc_remote_modbus_encode(sc_remote_sensor_config_t *sensor,
                             const sc_remote_modbus_config_t *value){
    memset(sensor->json_key,0,sizeof(sensor->json_key));
    memcpy(sensor->json_key,value,sizeof(*value));
}

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
    for(int i=0;i<SC_MAX_AUTOMATION_RULES;i++){
        c->modbus_rule[i].port=502;
        c->modbus_rule[i].unit=1;
        c->modbus_rule[i].send_clear=1;
    }
}

esp_err_t sc_config_init(void){
    lock=xSemaphoreCreateMutex(); if(!lock)return ESP_ERR_NO_MEM;
    sc_config_defaults(&cfg); nvs_handle h; esp_err_t e=nvs_open("smartcfg",NVS_READONLY,&h);
    if(e==ESP_OK){
        size_t n=0;e=nvs_get_blob(h,"config",NULL,&n);
        if(e==ESP_OK&&(n==sizeof(cfg)||n==offsetof(sc_config_t,modbus_rule)||n==offsetof(sc_config_t,remote_transport)||n==offsetof(sc_config_t,automation)||n==offsetof(sc_config_t,web_user)||n==offsetof(sc_config_t,modbus_unit_id)||n==offsetof(sc_config_t,input_count)||n==offsetof(sc_config_t,timer_schedule))){size_t got=n;e=nvs_get_blob(h,"config",&cfg,&got);}
        nvs_close(h);
        if(e==ESP_OK&&n==sizeof(cfg)&&cfg.version==CFG_VERSION){ESP_LOGI(TAG,"loaded configuration");return ESP_OK;}
        if(e==ESP_OK&&n==offsetof(sc_config_t,modbus_rule)&&cfg.version==CFG_VERSION_V8){
            cfg.version=CFG_VERSION;ESP_LOGI(TAG,"migrated configuration v8 to v9");return ESP_OK;
        }
        if(e==ESP_OK&&n==offsetof(sc_config_t,remote_transport)&&cfg.version==CFG_VERSION_V7){
            cfg.version=CFG_VERSION;ESP_LOGI(TAG,"migrated configuration v7 to v9");return ESP_OK;
        }
        if(e==ESP_OK&&n==offsetof(sc_config_t,automation)&&cfg.version==CFG_VERSION_V6){
            cfg.version=CFG_VERSION;ESP_LOGI(TAG,"migrated configuration v6 to v7");return ESP_OK;
        }
        if(e==ESP_OK&&n==offsetof(sc_config_t,web_user)&&cfg.version==CFG_VERSION_V5){
            cfg.version=CFG_VERSION;ESP_LOGI(TAG,"migrated configuration v5 to v7");return ESP_OK;
        }
        if(e==ESP_OK&&n==offsetof(sc_config_t,modbus_unit_id)&&cfg.version==CFG_VERSION_V4){
            cfg.version=CFG_VERSION;ESP_LOGI(TAG,"migrated configuration v4 to v7");return ESP_OK;
        }
        if(e==ESP_OK&&n==offsetof(sc_config_t,input_count)&&cfg.version==CFG_VERSION_V3){
            cfg.version=CFG_VERSION;ESP_LOGI(TAG,"migrated configuration v3 to v7");return ESP_OK;
        }
        if(e==ESP_OK&&n==offsetof(sc_config_t,timer_schedule)&&cfg.version==CFG_VERSION_V2){
            for(int i=0;i<SC_MAX_TIMERS;i++){uint32_t duration=cfg.timer[i].duration_sec;cfg.timer_schedule[i].duration_hours=duration/3600;cfg.timer_schedule[i].duration_minutes=(duration%3600)/60;uint32_t end=cfg.timer[i].start_hour*60+cfg.timer[i].start_minute+duration/60;cfg.timer_schedule[i].end_hour=(end/60)%24;cfg.timer_schedule[i].end_minute=end%60;}
            cfg.version=CFG_VERSION;ESP_LOGI(TAG,"migrated configuration v2 to v7");return ESP_OK;
        }
    }
    sc_config_defaults(&cfg);
    ESP_LOGW(TAG,"using defaults"); return ESP_OK;
}

esp_err_t sc_config_storage_ready(void){
    xSemaphoreTake(lock,portMAX_DELAY);
    esp_err_t e=load_file();
    if(e==ESP_OK)ESP_LOGI(TAG,"loaded filesystem configuration");
    else{
        e=persist_file(&cfg);
        if(e==ESP_OK)ESP_LOGI(TAG,"migrated configuration storage to LittleFS");
    }
    storage_ready=e==ESP_OK;
    xSemaphoreGive(lock);
    return e;
}
void sc_config_snapshot(sc_config_t *out){ xSemaphoreTake(lock,portMAX_DELAY); *out=cfg; xSemaphoreGive(lock); }
const sc_config_t *sc_config_acquire(void){ xSemaphoreTake(lock,portMAX_DELAY); return &cfg; }
void sc_config_release(void){ xSemaphoreGive(lock); }
esp_err_t sc_config_replace(const sc_config_t *in){if(!in||in->version!=CFG_VERSION)return ESP_ERR_INVALID_ARG;xSemaphoreTake(lock,portMAX_DELAY);esp_err_t e=storage_ready?persist_file(in):ESP_ERR_INVALID_STATE;if(e==ESP_OK)cfg=*in;else ESP_LOGE(TAG,"configuration save failed: %s",esp_err_to_name(e));xSemaphoreGive(lock);return e;}

esp_err_t sc_config_set_relay_timing(uint8_t relay, bool ttl,
                                     uint32_t seconds)
{
    if (relay >= SC_MAX_RELAYS) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (relay >= cfg.relay_count) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t *value = ttl ? &cfg.relay[relay].ttl_sec
                          : &cfg.relay[relay].tta_sec;
    if (*value == seconds) {
        xSemaphoreGive(lock);
        return ESP_OK;
    }
    uint32_t previous = *value;
    *value = seconds;
    esp_err_t err = storage_ready ? persist_file(&cfg) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        *value = previous;
        ESP_LOGE(TAG, "relay timing save failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(lock);
    return err;
}
