#include <stdint.h>
#include "esp_heap_caps.h"
#include "esp_heap_port.h"
#include "priv/esp_heap_caps_priv.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sc_health.h"

static volatile sc_health_snapshot_t health;
static bool heap_walk_warning_logged;
static const char *TAG = "health";

static bool block_address_valid(const heap_region_t *region,
                                const mem_blk_t *block)
{
    uintptr_t address = (uintptr_t)block;
    uintptr_t start = HEAP_ALIGN((uintptr_t)region->start_addr);
    uintptr_t end = (uintptr_t)region->start_addr + region->total_size;
    return address >= start &&
           address <= end - MEM_HEAD_SIZE &&
           (address & (HEAP_ALIGN_SIZE - 1)) == 0;
}

static uint32_t largest_free_block(void)
{
    extern heap_region_t g_heap_region[];
    extern size_t g_heap_region_num;
    size_t largest = 0;
    bool valid = true;
    for (size_t region = 0; region < g_heap_region_num; region++) {
        if (!(g_heap_region[region].caps & MALLOC_CAP_8BIT)) continue;
        _heap_caps_lock(region);
        mem_blk_t *block = (mem_blk_t *)g_heap_region[region].free_blk;
        while (block) {
            if (!block_address_valid(&g_heap_region[region], block)) {
                valid = false;
                break;
            }
            mem_blk_t *next = mem_blk_next(block);
            if (!next) break;
            if (!block_address_valid(&g_heap_region[region], next) ||
                (uintptr_t)next <= (uintptr_t)block) {
                valid = false;
                break;
            }
            if (mem_blk_prev(next) != block) {
                valid = false;
                break;
            }
            if (!mem_blk_is_used(block)) {
                size_t linked = (uintptr_t)next - (uintptr_t)block;
                size_t header = mem_blk_head_size(mem_blk_is_traced(block));
                size_t size = linked > header ? linked - header : 0;
                if (size > largest) largest = size;
            }
            block = next;
        }
        _heap_caps_unlock(region);
        if (!valid) break;
    }
    if (!valid) {
        if (!heap_walk_warning_logged) {
            heap_walk_warning_logged = true;
            ESP_LOGE(TAG, "invalid heap metadata; largest block unavailable");
        }
        return 0;
    }
    return (uint32_t)largest;
}

esp_err_t sc_health_init(void)
{
    health.free_heap = esp_get_free_heap_size();
    health.min_free_heap = health.free_heap;
    return ESP_OK;
}

void sc_health_snapshot(sc_health_snapshot_t *out)
{
    if (!out) return;
    sc_health_snapshot_t snapshot = health;
    snapshot.free_heap = esp_get_free_heap_size();
    if (!health.min_free_heap || snapshot.free_heap < health.min_free_heap)
        health.min_free_heap = snapshot.free_heap;
    snapshot.min_free_heap = health.min_free_heap;
    snapshot.max_free_block = largest_free_block();
    *out = snapshot;
}

void sc_health_wifi_reconnect(void) { health.wifi_reconnects++; }
void sc_health_mqtt_reconnect(void) { health.mqtt_reconnects++; }
void sc_health_http_request(void) { health.http_requests++; }
void sc_health_relay_command(void) { health.relay_commands++; }
