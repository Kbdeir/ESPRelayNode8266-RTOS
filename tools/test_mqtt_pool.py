"""Check production MQTT payload storage/backpressure without network or RTOS."""
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
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define portMAX_DELAY 0xffff
#define ESP_LOGW(...) ((void)0)
typedef uint32_t TickType_t;
static void *pool_lock = (void *)1, *pq = (void *)1, *client = (void *)1;
static bool connected = true, lock_available = true, queue_available = true;
static TickType_t now;
static int delays, semaphore_wait, queue_wait;
static uint8_t queued_slot;
static void *pub_task;
static int watermark_calls;
static uint32_t uxTaskGetStackHighWaterMark(void *task) {
    assert(task == pub_task && task != NULL);
    watermark_calls++;
    return 704;
}
static int xSemaphoreTake(void *lock, int wait) {
    (void)lock; semaphore_wait = wait; return lock_available;
}
static void xSemaphoreGive(void *lock) { (void)lock; }
static TickType_t xTaskGetTickCount(void) { return now; }
static void vTaskDelay(int ms) { delays++; now += ms; }
'''
CHECKS = r'''
int main(void) {
    assert(sc_mqtt_publisher_stack_free() == 0 && watermark_calls == 0);
    pub_task = (void *)1;
    assert(sc_mqtt_publisher_stack_free() == 704 && watermark_calls == 1);
    assert(sizeof(small_payload) + sizeof(large_payload) == 768);
    char big[PUB_PAYLOAD_LEN], too_big[PUB_PAYLOAD_LEN + 1];
    memset(big, 'x', sizeof(big) - 1); big[sizeof(big) - 1] = 0;
    memset(too_big, 'x', sizeof(too_big) - 1); too_big[sizeof(too_big) - 1] = 0;
    for (int i = 0; i < 3; i++) {
        assert(payload_acquire("25.06", false) == i);
        assert(!strcmp(payload_buffer(i), "25.06"));
    }
    assert(payload_acquire(big, false) == 3);
    assert(!strcmp(payload_buffer(3), big));
    assert(payload_acquire("0", false) == -1 && delays == 0);
    assert(payload_acquire(too_big, false) == -1);
    for (int i = 0; i < PUB_SLOTS; i++) payload_release(i);
    assert(queue_publish("temp", "18.50", true, 0));
    assert(queue_wait == 0 && semaphore_wait == 0 && delays == 0);
    assert(!strcmp(payload_buffer(queued_slot), "18.50"));
    payload_release(queued_slot);
    assert(queue_publish("ha/config", big, true, 1));
    assert(queued_slot == 3 && queue_wait == 100);
    payload_release(queued_slot);
    queue_available = false;
    assert(!queue_publish("temp", "20", true, 0));
    for (int i = 0; i < PUB_SLOTS; i++) assert(!payload_used[i]);
    queue_available = true; lock_available = false;
    assert(!queue_publish("temp", "20", true, 0) && delays == 0);
    assert(payload_acquire("on", true) == -1 && delays > 0 && now >= 250);
    lock_available = true;
    assert(payload_acquire(big, true) == 3);
    TickType_t before = now;
    assert(payload_acquire(big, true) == -1 && now - before >= 250);
    assert(payload_acquire("on", false) == 0); // Short telemetry does not need the large slot.
    payload_release(0);
    payload_release(3);
    payload_release(PUB_SLOTS);
    delays = 0;
    assert(sc_mqtt_try_publish("ha/config", big, true));
    assert(queued_slot == 3 && queue_wait == 0 && semaphore_wait == 0);
    assert(!sc_mqtt_try_publish("ha/next", big, true) && delays == 0);
    assert(sc_mqtt_try_publish("short", "on", true));
    payload_release(queued_slot);
    payload_release(3);
    queue_available = false;
    assert(!sc_mqtt_try_publish("ha/config", big, true));
    for (int i = 0; i < PUB_SLOTS; i++) assert(!payload_used[i]);
    puts("PASS: 768-byte MQTT pool, four short messages, full-length discovery, "
         "slot reuse, queue failure cleanup, nonblocking telemetry and task watermark");
}
'''


def main():
    source = (ROOT / "components/sc_mqtt/sc_mqtt.c").read_text(encoding="utf-8")
    types = source[source.index("#define PUB_SLOTS"):source.index("typedef struct {\n    uint8_t mqtt_active;")]
    pool = source[source.index("static char large_payload"):source.index("static volatile bool connection_sync_pending;")]
    logic = source[source.index("static char *payload_buffer("):source.index("static bool publish_direct(")]
    queue = source[source.index("static bool queue_publish_once("):source.index("bool sc_mqtt_publish(")]
    watermark = source[source.index("uint32_t sc_mqtt_publisher_stack_free("):
                       source.index("bool sc_mqtt_connected(")]
    send = r'''
static int xQueueSend(void *q, const pub_t *item, int wait) {
    (void)q; queue_wait = wait; queued_slot = item->slot; return queue_available;
}
'''
    with tempfile.TemporaryDirectory(prefix="mqtt-pool-") as temp:
        work = Path(temp)
        (work / "test.c").write_text(MOCKS + types + pool + send + logic + queue + watermark + CHECKS, encoding="utf-8")
        prefix = ["wsl", "--cd", str(work), "--exec"] if os.name == "nt" else []
        subprocess.run(prefix + ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "test.c", "-o", "test"], cwd=work, check=True)
        subprocess.run(prefix + ["./test"], cwd=work, check=True)


if __name__ == "__main__":
    main()
