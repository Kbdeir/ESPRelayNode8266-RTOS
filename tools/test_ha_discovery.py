"""Run the production HA worker against a slow single-large-slot MQTT queue."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SC_TOPIC_LEN 96
#define SC_LOCATION_LEN 64
#define SC_MAX_RELAYS 2
#define SC_MAX_INPUTS 5
#define SC_INPUT_TEMPERATURE 4
#define ESP_MAC_WIFI_STA 0
#define ESP_OK 0
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_LOGW(tag,...) ((void)(tag))
#define ESP_LOGE(tag,...) ((void)(tag))
typedef uint32_t TickType_t;
typedef int esp_err_t;
typedef struct {char sub_topic[96], state_pub_topic[96], pub_topic[96], acs_topic[96]; bool acs_enabled;} relay_config_t;
typedef struct {uint8_t mode; char topic[96];} input_config_t;
typedef struct {
    uint8_t relay_count, input_count; bool ha_discovery_active, ds18b20_active;
    char phy_loc[64], temp_topic1[96], temp_topic2[96];
    relay_config_t relay[2]; input_config_t input[5];
} sc_config_t;
typedef struct {bool connected; char health_topic[96];} sc_mqtt_status_t;
static sc_config_t config;
static TickType_t now, busy_until;
static bool connected=true, stuck, disconnect_on_delay, disable_on_delay;
static bool request_during_publish, task_create_ok=true;
static unsigned mac_reads, accepted, attempts, created, deleted;
static int fail_after=-1;
static char topics[64][96], payloads[64][672];
void sc_ha_request_publish(void);
static const sc_config_t *sc_config_acquire(void) {return &config;}
static void sc_config_release(void) {}
static bool sc_mqtt_connected(void) {return connected;}
static void sc_mqtt_status(sc_mqtt_status_t *status) {
    status->connected=connected;
    snprintf(status->health_topic, sizeof(status->health_topic), "node/Birth");
}
static void esp_read_mac(uint8_t *mac, int interface) {
    (void)interface; const uint8_t value[]={0xA0,0x20,0xA6,0x14,0x3C,0xC6};
    memcpy(mac,value,6); mac_reads++;
}
static TickType_t xTaskGetTickCount(void) {return now;}
static void vTaskDelay(int ms) {
    now += ms;
    if(disconnect_on_delay) connected=false;
    if(disable_on_delay) config.ha_discovery_active=false;
}
static bool sc_mqtt_try_publish(const char *topic, const char *payload, bool retain) {
    assert(retain); attempts++;
    if(request_during_publish) {request_during_publish=false; sc_ha_request_publish();}
    if(stuck || now < busy_until || (fail_after>=0 && accepted==(unsigned)fail_after)) return false;
    assert(accepted < 64);
    snprintf(topics[accepted],96,"%s",topic);
    snprintf(payloads[accepted++],672,"%s",payload);
    // Slow network: the only large slot stays occupied for 1.2 seconds.
    busy_until=now+1200;
    return true;
}
static int xTaskCreate(void (*fn)(void *), const char *name, int stack,
                       void *arg, int priority, void *handle) {
    (void)fn; (void)name; (void)arg; (void)priority; (void)handle;
    assert(stack==3328); created++; return task_create_ok;
}
static void vTaskDelete(void *handle) {(void)handle; deleted++;}
'''
CHECKS = r'''
static void reset(void) {
    memset(&config,0,sizeof(config));
    config.relay_count=1; config.input_count=1; config.ha_discovery_active=true;
    snprintf(config.phy_loc,64,"Controller");
    snprintf(config.relay[0].sub_topic,96,"relay/set");
    snprintf(config.relay[0].state_pub_topic,96,"relay/state");
    snprintf(config.relay[0].pub_topic,96,"relay");
    snprintf(config.relay[0].acs_topic,96,"relay/amps");
    snprintf(config.input[0].topic,96,"input/1");
    snprintf(config.temp_topic1,96,"temp/1");
    snprintf(config.temp_topic2,96,"temp/2");
    now=busy_until=accepted=attempts=mac_reads=created=deleted=0;
    connected=task_create_ok=true;
    stuck=disconnect_on_delay=disable_on_delay=request_during_publish=false;
    fail_after=-1;
    worker_active=publish_requested=false;
}
int main(void) {
    reset();
    sc_ha_request_publish();
    assert(worker_active && created==1);
    task(NULL);
    assert(!worker_active && deleted==1 && accepted==6);
    assert(attempts>accepted && mac_reads==1);
    assert(strstr(topics[0],"/switch/") && strstr(topics[0],"/relay0/config"));
    assert(strstr(topics[1],"/sensor/") && strstr(topics[1],"/ttl0/config") && !payloads[1][0]);
    assert(strstr(topics[2],"/number/") && strstr(topics[2],"/ttl0/config"));
    assert(strstr(topics[3],"/sensor/") && strstr(topics[3],"/tta0/config") && !payloads[3][0]);
    assert(strstr(topics[4],"/number/") && strstr(topics[4],"/tta0/config"));
    assert(strstr(topics[5],"/binary_sensor/") && strstr(topics[5],"/input1/config"));
    for(unsigned i=0;i<accepted;i++) for(unsigned j=0;j<i;j++) assert(strcmp(topics[i],topics[j]));

    reset(); config.ds18b20_active=true; config.relay[0].acs_enabled=true;
    assert(publish_all() && accepted==9 && mac_reads==1);
    assert(strstr(topics[5],"current0/config"));
    assert(strstr(topics[7],"temperature1/config"));
    assert(strstr(topics[8],"temperature2/config"));

    reset(); fail_after=2; sc_ha_request_publish(); task(NULL);
    assert(accepted==2 && mac_reads==1 && deleted==1 && !worker_active);
    assert(strstr(topics[0],"relay0/config") && strstr(topics[1],"ttl0/config"));

    reset(); stuck=true; sc_ha_request_publish(); task(NULL);
    assert(now==10000 && accepted==0 && attempts==100 && deleted==1 && !worker_active);
    stuck=false; sc_ha_request_publish(); task(NULL);
    assert(accepted==6 && created==2 && deleted==2);

    reset(); busy_until=500; disconnect_on_delay=true;
    sc_ha_request_publish(); task(NULL);
    assert(accepted==0 && now==100 && deleted==1 && !worker_active);
    reset(); busy_until=500; disable_on_delay=true;
    sc_ha_request_publish(); task(NULL);
    assert(accepted==0 && now==100 && deleted==1 && !worker_active);

    reset(); request_during_publish=true;
    sc_ha_request_publish(); task(NULL);
    assert(accepted==12 && mac_reads==2 && created==1 && deleted==1 && !publish_requested);
    reset(); task_create_ok=false;
    sc_ha_request_publish(); assert(created==1 && !worker_active);

    reset(); now=UINT32_MAX-5000; stuck=true;
    TickType_t before=now;
    assert(!pub("switch","node","relay0","{}"));
    assert((TickType_t)(now-before)==10000);
    char invalid[672]; memset(invalid,'x',671); invalid[671]=0;
    assert(!pub("switch",invalid,"relay0","{}") && attempts==100);
    puts("PASS: paced HA discovery, one MAC read, no batch replay on congestion, bounded timeout, cancel/reconnect and worker cleanup");
}
'''


def main():
    source = (ROOT / "components/sc_ha/sc_ha.c").read_text(encoding="utf-8")
    source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
    with tempfile.TemporaryDirectory(prefix="ha-discovery-") as temp:
        work = Path(temp)
        (work / "test.c").write_text(MOCKS + source + CHECKS, encoding="utf-8")
        prefix = ["wsl", "--cd", str(work), "--exec"] if os.name == "nt" else []
        subprocess.run(prefix + ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "test.c", "-o", "test"], cwd=work, check=True)
        subprocess.run(prefix + ["./test"], cwd=work, check=True)


if __name__ == "__main__":
    main()
