"""Characterize the pinned SDK's outbox limit; PASS confirms a release blocker.

This is a diagnostic, not a passing firmware acceptance test. Compile the
installed SDK implementation with host mocks and attempt to exceed its nominal
4 KiB limit using unacknowledged QoS messages.
"""
import os
from pathlib import Path
import subprocess
import tempfile

SDK = (Path.home() / ".platformio/packages/framework-esp8266-rtos-sdk"
       / "components/mqtt/esp-mqtt/lib/mqtt_outbox.c")

MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
// glibc's BSD queue header omits the safe iterator provided by the SDK.
#include <sys/queue.h>
#ifndef STAILQ_FOREACH_SAFE
#define STAILQ_FOREACH_SAFE(item, head, field, next_item) \
    for ((item) = STAILQ_FIRST(head); \
         (item) && ((next_item) = STAILQ_NEXT(item, field), 1); \
         (item) = (next_item))
#endif
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_MEM_CHECK(tag, value, action) do { if (!(value)) { action; } } while (0)
#define ESP_LOGD(...) ((void)0)
typedef long long outbox_tick_t;
typedef enum {QUEUED, TRANSMITTED, ACKNOWLEDGED, CONFIRMED} pending_state_t;
typedef struct outbox_list_t *outbox_handle_t;
typedef struct outbox_item *outbox_item_handle_t;
typedef struct {
    const char *data;
    int len, msg_id, msg_type, msg_qos;
    const char *remaining_data;
    int remaining_len;
} outbox_message_t;
typedef outbox_message_t *outbox_message_handle_t;
int outbox_get_size(outbox_handle_t outbox);
'''

CHECKS = r'''
int main(void) {
    outbox_handle_t box = outbox_init();
    assert(box);
    char data[672] = {0};
    for (int i = 0; i < 7; i++) {
        outbox_message_t message = {
            .data=data, .len=sizeof(data), .msg_id=i + 1,
            .msg_type=3, .msg_qos=1
        };
        assert(outbox_enqueue(box, &message, 1000));
        assert(outbox_set_pending(box, i + 1, TRANSMITTED) == ESP_OK);
    }
    assert(outbox_get_size(box) == 4704);
    assert(outbox_cleanup(box, 4096) == ESP_FAIL);
    assert(outbox_get_size(box) == 4704);
    assert(outbox_delete_expired(box, 2000, 30000) == 0);
    // Backward wall-clock corrections delay rather than accelerate expiry.
    assert(outbox_delete_expired(box, 0, 30000) == 0);
    assert(outbox_delete_expired(box, 31001, 30000) == 7);
    assert(outbox_get_size(box) == 0);
    outbox_destroy(box);
    puts("CONFIRMED: nominal 4096-byte SDK outbox limit permits 4704 bytes of "
         "unacknowledged messages; expiry is eventual, not a hard memory cap.");
    return 0;
}
'''


def main():
    source = SDK.read_text()
    source = source.replace('#include "mqtt_outbox.h"', MOCKS)
    source = source.replace('#include "esp_log.h"', "")
    with tempfile.TemporaryDirectory(prefix="sc-outbox-audit-") as temporary:
        directory = Path(temporary)
        (directory / "audit.c").write_text(source + CHECKS)
        if os.name == "nt":
            native = str(directory.resolve())
            drive, tail = native.split(":", 1)
            linux = "/mnt/" + drive.lower() + tail.replace("\\", "/")
            subprocess.run([
                "wsl", "sh", "-lc",
                f"cd '{linux}' && cc -std=c99 -Wall -Wextra -Wno-unused-variable "
                "audit.c -o audit && ./audit"
            ], check=True)
        else:
            subprocess.run(["cc", "-std=c99", "audit.c", "-o", "audit"],
                           cwd=directory, check=True)
            subprocess.run([str(directory / "audit")], check=True)


if __name__ == "__main__":
    main()
