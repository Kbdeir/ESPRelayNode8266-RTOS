"""Exercise production relay transitions and MQTT delivery with delayed echoes."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def function(source, signature):
    start = source.index(signature)
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#define SC_TOPIC_LEN 96
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
typedef int esp_err_t;
typedef enum {SC_RELAY_SET, SC_RELAY_TOGGLE} sc_relay_op_t;
typedef struct {uint8_t relay; sc_relay_op_t op; bool value;} sc_relay_cmd_t;
typedef struct {char sub_topic[96], pub_topic[96], state_pub_topic[96];} relay_config_t;
typedef struct {uint8_t relay_count; relay_config_t relay[2];} sc_config_t;
static sc_config_t config = {.relay_count=1, .relay={{"relay/set", "relay", "relay/state"}}};
static sc_config_t cfg = {.relay_count=1};
static bool state[2], connected, timing_commands_ready, connection_sync_pending;
static int64_t ttl_deadline_us[2], tta_deadline_us[2];
static uint8_t relay_count=1;
static struct {uint8_t gpio; bool active_high; uint32_t ttl_sec, tta_sec;} relay_cfg[2];
static sc_relay_cmd_t commands[128];
static int head, tail, transitions;
static bool values[128];
static int emitted, received;
static struct {char topic[96], payload[8];} messages[128];
static const sc_config_t *sc_config_acquire(void) {return &config;}
static void sc_config_release(void) {}
static void gpio_set_level(int gpio, bool value) {(void)gpio; (void)value;}
static void sc_relay_state_changed(uint8_t relay, bool value) {
    (void)relay; assert(transitions < 128); values[transitions++]=value;
}
static bool sc_relay_submit(const sc_relay_cmd_t *cmd) {
    assert(tail < 128); commands[tail++]=*cmd; return true;
}
static bool queue_timing_update(uint8_t relay, bool ttl, uint32_t seconds) {
    (void)relay; (void)ttl; (void)seconds; return true;
}
static void sc_health_mqtt_reconnect(void) {}
static bool sc_automation_remote_message(const char *topic, size_t topic_len,
                                        const char *payload, size_t payload_len) {
    (void)topic; (void)topic_len; (void)payload; (void)payload_len; return false;
}
enum {MQTT_EVENT_CONNECTED, MQTT_EVENT_DISCONNECTED, MQTT_EVENT_DATA};
typedef struct {int event_id; const char *topic, *data; int topic_len, data_len;} event_t;
typedef event_t *esp_mqtt_event_handle_t;
static bool publish_direct(const char *topic, const char *payload, int qos) {
    (void)qos; assert(emitted < 128);
    snprintf(messages[emitted].topic, 96, "%s", topic);
    snprintf(messages[emitted++].payload, 8, "%s", payload); return true;
}
'''

CHECKS = r'''
static void incoming(const char *topic, const char *payload) {
    event_t e={MQTT_EVENT_DATA, topic, payload, (int)strlen(topic), (int)strlen(payload)};
    assert(event_cb(&e)==ESP_OK);
}
static void drain(int64_t now) {
    while(head < tail) {
        handle_command(&commands[head++], now);
        // Publish captured transitions later, just like the MQTT worker.
    }
    while(received < emitted) {
        int i=received++;
        incoming(messages[i].topic, messages[i].payload);
    }
}
int main(void) {
    connected=true;
    incoming("relay/set", "on"); drain(1000000);
    incoming("relay/set", "off"); drain(2000000);
    incoming("relay/set", "on"); drain(3000000);
    assert(state[0] && transitions==3 && tail==3);
    // A slow publisher sends obsolete OFF after a newer ON is already applied.
    assert(publish_relay_state_direct(0, false));
    assert(publish_relay_state_direct(0, true));
    drain(4000000);
    assert(tail==3 && transitions==3 && state[0]);
    assert(emitted==4);
    for(int i=0;i<emitted;i++) assert(strcmp(messages[i].topic,"relay/set"));
    assert(!strcmp(messages[0].topic,"relay/state"));
    assert(!strcmp(messages[1].topic,"relay"));
    incoming("relay/set", "tog"); drain(5000000);
    assert(!state[0] && transitions==4);
    incoming("relay/set", "ON"); drain(6000000);
    assert(state[0] && transitions==5);
    incoming("relay/set", "OFF"); drain(7000000);
    assert(!state[0] && transitions==6);
    relay_cfg[0].tta_sec=2; relay_cfg[0].ttl_sec=1;
    incoming("relay/set", "on"); drain(8000000);
    assert(!state[0] && tta_deadline_us[0]==10000000);
    process_deadlines(10000000);
    assert(state[0] && ttl_deadline_us[0]==11000000);
    process_deadlines(11000000);
    assert(!state[0] && !tta_deadline_us[0]);
    assert(publish_relay_state_direct(0, true)); // delayed activation notification
    assert(publish_relay_state_direct(0, false));
    drain(12000000);
    process_deadlines(20000000);
    assert(!state[0] && !tta_deadline_us[0] && transitions==8);
    assert(!publish_relay_state_direct(1, true));
    puts("PASS: delayed relay MQTT state cannot echo commands or restart TTL/TTA; external commands still work");
}
'''


def main():
    mqtt = (ROOT / "components/sc_mqtt/sc_mqtt.c").read_text(encoding="utf-8")
    relay = (ROOT / "components/sc_relay/sc_relay.c").read_text(encoding="utf-8")
    logic = "".join(function(relay, sig) for sig in [
        "static void write_relay(", "static int64_t deadline_after(",
        "static void set_deadlines(", "static void set_relay_now(",
        "static void request_relay_on(", "static void handle_command(",
        "static void process_deadlines("])
    logic += "".join(function(mqtt, sig) for sig in [
        "static int topic_eq(", "static bool parse_seconds(",
        "static bool publish_relay_state_direct(", "static esp_err_t event_cb("])
    with tempfile.TemporaryDirectory(prefix="relay-mqtt-") as temp:
        work = Path(temp)
        (work / "test.c").write_text(MOCKS + logic + CHECKS, encoding="utf-8")
        prefix = ["wsl", "--cd", str(work), "--exec"] if os.name == "nt" else []
        subprocess.run(prefix + ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "test.c", "-o", "test"], cwd=work, check=True)
        subprocess.run(prefix + ["./test"], cwd=work, check=True)


if __name__ == "__main__":
    main()
