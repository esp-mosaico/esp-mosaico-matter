/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

/* One hot-pluggable Mosaico module type that maps onto one or more native
 * Matter devices. Add a new accessory by implementing this table, listing it
 * in mosaico_hotplug_bridge.c's s_accessories[], and adding its sources to
 * CMakeLists.txt -- the dispatcher, queue and Matter helpers stay shared. */
typedef struct {
    const char *name;
    uint8_t board_type;
    /* Open the module and add its Matter device(s). Return ESP_OK only if
     * this accessory now owns `event->slot`; the dispatcher then calls
     * on_absent on unplug. On failure the accessory must leave the slot
     * idle (no open handle, no leftover devices). */
    esp_err_t (*on_present)(const mosaico_hotplug_event_t *event);
    void (*on_absent)(const mosaico_hotplug_event_t *event);
    /* Polls on the hot-plug task. May wait for Matter operations. */
    void (*poll)(mosaico_module_mgr_slot_t slot, int64_t now_ms);
    /* Runs on the Matter task: must not wait for Matter or access hardware. */
    void (*attribute_changed)(const bridge_dev_mngr_attribute_change_t *change);
} mosaico_hotplug_accessory_t;

extern const mosaico_hotplug_accessory_t mosaico_hotplug_interact_accessory;
extern const mosaico_hotplug_accessory_t mosaico_hotplug_camera_accessory;

/* Shared Matter bookkeeping for accessory implementations. */
void mosaico_hotplug_devices_init(bridge_dev_mngr_handle_t dev_mngr);
esp_err_t mosaico_hotplug_get_devices(bridge_dev_mngr_device_info_t *devices,
    size_t capacity, size_t *count);
esp_err_t mosaico_hotplug_register_device(const bridge_dev_mngr_device_definition_t *definition);
esp_err_t mosaico_hotplug_activate_device(const char *id, bridge_dev_mngr_device_info_t *out);
esp_err_t mosaico_hotplug_deactivate_device(const char *id);
esp_err_t mosaico_hotplug_remove_device(const char *id);
esp_err_t mosaico_hotplug_set_reachable(const char *id, bool reachable);
esp_err_t mosaico_hotplug_set_name(const char *id, const char *name);
esp_err_t mosaico_hotplug_read(const char *id, const bridge_dev_mngr_attribute_path_t *path,
    bridge_dev_mngr_value_t *value);
esp_err_t mosaico_hotplug_report(const char *id, const bridge_dev_mngr_attribute_path_t *path,
    const bridge_dev_mngr_value_t *value);

#ifdef __cplusplus
}
#endif
