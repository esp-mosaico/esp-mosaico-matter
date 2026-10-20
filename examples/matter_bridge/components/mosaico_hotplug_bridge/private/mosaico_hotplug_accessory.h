/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"
#include "bridge_dev_mngr.h"
#include "mosaico_module_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One PRESENT/ABSENT notification delivered on the hot-plug task. board_id
 * and serial_number are valid only when presence is PRESENT. */
typedef struct {
    mosaico_module_mgr_slot_t slot;
    mosaico_module_presence_t presence;
    uint8_t board_type;
    uint16_t board_id;
    uint32_t serial_number;
} mosaico_hotplug_event_t;

/* One hot-pluggable Mosaico module type that maps onto one or more virtual
 * Matter devices. Add a new accessory by implementing this table, listing it
 * in mosaico_hotplug_bridge.c's s_accessories[], and adding its sources to
 * CMakeLists.txt -- the dispatcher, queue and Matter helpers stay shared. */
typedef struct {
    const char *name;
    uint8_t board_type;
    esp_err_t (*register_lua)(void);
    /* Open the module and add its Matter device(s). Return ESP_OK only if
     * this accessory now owns `event->slot`; the dispatcher then calls
     * on_absent on unplug. On failure the accessory must leave the slot
     * idle (no open handle, no leftover devices). */
    esp_err_t (*on_present)(const mosaico_hotplug_event_t *event);
    void (*on_absent)(const mosaico_hotplug_event_t *event);
} mosaico_hotplug_accessory_t;

extern const mosaico_hotplug_accessory_t mosaico_hotplug_interact_accessory;
extern const mosaico_hotplug_accessory_t mosaico_hotplug_camera_accessory;

/* Shared Matter bookkeeping for accessory implementations. */
cJSON *mosaico_hotplug_fetch_devices(void);
bool mosaico_hotplug_find_existing_device(cJSON *devices_list, const char *name,
    char *out_id, size_t out_id_len);
/* Consumes `driver_args` (NULL becomes {}). */
bool mosaico_hotplug_add_virtual_device(const char *name, const char *type,
    const char *driver_source, cJSON *driver_args, char *out_id, size_t out_id_len);
void mosaico_hotplug_remove_device(const char *id);

#ifdef __cplusplus
}
#endif
