/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "bridge_memory.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

/* The allocation hook must not allocate, print, or inspect the heap.
 * Only copy/update counters in this short ISR-safe critical section.
 * The lock and counters stay internal; a task reports them later.
 */
typedef struct {
    unsigned failures, last_size, last_caps, reported;
} failure_stats_t;
static DRAM_ATTR failure_stats_t s_stats;
static DRAM_ATTR portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR allocation_failed(size_t size, uint32_t caps, const char *function)
{
    (void)function;
    portENTER_CRITICAL_SAFE(&s_lock);
    s_stats.last_size = (unsigned)size;
    s_stats.last_caps = caps;
    s_stats.failures++;
    portEXIT_CRITICAL_SAFE(&s_lock);
}

esp_err_t bridge_memory_init(void)
{
    return heap_caps_register_failed_alloc_callback(allocation_failed);
}

void bridge_memory_log(const char *phase)
{
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const uint32_t dma = MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    failure_stats_t stats;
    portENTER_CRITICAL_SAFE(&s_lock);
    stats = s_stats;
    s_stats.reported = stats.failures;
    portEXIT_CRITICAL_SAFE(&s_lock);
    ESP_LOGI("bridge_heap",
             "%s internal free=%u min=%u largest=%u; DMA free=%u largest=%u; PSRAM free=%u min=%u largest=%u; alloc_failures=%u",
             phase, (unsigned)heap_caps_get_free_size(internal),
             (unsigned)heap_caps_get_minimum_free_size(internal),
             (unsigned)heap_caps_get_largest_free_block(internal),
             (unsigned)heap_caps_get_free_size(dma),
             (unsigned)heap_caps_get_largest_free_block(dma),
             (unsigned)heap_caps_get_free_size(external),
             (unsigned)heap_caps_get_minimum_free_size(external),
             (unsigned)heap_caps_get_largest_free_block(external), stats.failures);
    if (stats.failures != stats.reported) {
        ESP_LOGW("bridge_heap", "Last failed allocation: size=%u caps=0x%x",
                 stats.last_size, stats.last_caps);
    }
}
