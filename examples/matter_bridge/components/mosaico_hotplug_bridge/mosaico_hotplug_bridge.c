/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "mosaico_hotplug_bridge.h"
#include "mosaico_hotplug_accessory.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mosaico_module_mgr.h"

static const char *TAG = "mosaico_hotplug";

/* Register new Mosaico accessories here. Each entry is one board_type. */
static const mosaico_hotplug_accessory_t *const s_accessories[] = {
    &mosaico_hotplug_interact_accessory,
    &mosaico_hotplug_camera_accessory,
};

static QueueHandle_t s_events;
static const mosaico_hotplug_accessory_t *s_slot_accessory[MOSAICO_MODULE_MGR_SLOT_COUNT];

static const mosaico_hotplug_accessory_t *find_accessory(uint8_t board_type)
{
    for (size_t i = 0; i < sizeof(s_accessories) / sizeof(s_accessories[0]); i++) {
        if (s_accessories[i]->board_type == board_type) return s_accessories[i];
    }
    return NULL;
}

static void handle_present(const mosaico_hotplug_event_t *event)
{
    if (s_slot_accessory[event->slot]) {
        ESP_LOGW(TAG, "Ignoring duplicate PRESENT for %s slot (already active: %s)",
            mosaico_module_mgr_slot_to_name(event->slot), s_slot_accessory[event->slot]->name);
        return;
    }
    const mosaico_hotplug_accessory_t *accessory = find_accessory(event->board_type);
    if (!accessory) return;
    if (accessory->on_present(event) == ESP_OK) {
        s_slot_accessory[event->slot] = accessory;
    }
}

static void handle_absent(const mosaico_hotplug_event_t *event)
{
    const mosaico_hotplug_accessory_t *accessory = s_slot_accessory[event->slot];
    if (!accessory) return;
    accessory->on_absent(event);
    s_slot_accessory[event->slot] = NULL;
}

static void bridge_task(void *arg)
{
    (void)arg;
    mosaico_hotplug_event_t event;
    while (true) {
        TickType_t timeout = portMAX_DELAY;
        for (int slot = 0; slot < MOSAICO_MODULE_MGR_SLOT_COUNT; ++slot) {
            if (s_slot_accessory[slot] && s_slot_accessory[slot]->poll) timeout = pdMS_TO_TICKS(100);
        }
        if (xQueueReceive(s_events, &event, timeout) == pdTRUE) {
            if (event.presence == MOSAICO_MODULE_PRESENCE_PRESENT) {
                handle_present(&event);
            } else {
                handle_absent(&event);
            }
        }
        const int64_t now_ms = esp_timer_get_time() / 1000;
        for (int slot = 0; slot < MOSAICO_MODULE_MGR_SLOT_COUNT; ++slot) {
            const mosaico_hotplug_accessory_t *accessory = s_slot_accessory[slot];
            if (accessory && accessory->poll) accessory->poll((mosaico_module_mgr_slot_t)slot, now_ms);
        }
    }
}

/* Runs on the module manager's own event task: must stay short and
 * non-blocking. */
static void enqueue_event(const mosaico_module_mgr_info_t *info)
{
    if (info->presence != MOSAICO_MODULE_PRESENCE_PRESENT &&
        info->presence != MOSAICO_MODULE_PRESENCE_ABSENT) {
        return;
    }
    mosaico_hotplug_event_t event = {
        .slot = info->slot,
        .presence = info->presence,
        .board_type = info->eeprom.board_type,
    };
    if (info->presence == MOSAICO_MODULE_PRESENCE_PRESENT) {
        if (info->descriptor_state != MOSAICO_MODULE_DESCRIPTOR_VALID) return;
        if (!find_accessory(info->eeprom.board_type)) return;
        event.board_id = info->eeprom.board_id;
        event.serial_number = info->eeprom.serial_number;
    }
    if (s_events) xQueueSend(s_events, &event, 0);
}

static void on_module_event(const mosaico_module_mgr_event_t *event, void *user_data)
{
    (void)user_data;
    if (event && (event->changes & (MOSAICO_MODULE_CHANGE_PRESENCE | MOSAICO_MODULE_CHANGE_DESCRIPTOR))) {
        enqueue_event(&event->info);
    }
}

static void attribute_changed(bridge_dev_mngr_handle_t dev_mngr,
    const bridge_dev_mngr_attribute_change_t *change, void *ctx)
{
    (void)dev_mngr;
    (void)ctx;
    for (size_t i = 0; i < sizeof(s_accessories) / sizeof(s_accessories[0]); i++) {
        if (s_accessories[i]->attribute_changed) s_accessories[i]->attribute_changed(change);
    }
}

esp_err_t mosaico_hotplug_bridge_start(bridge_dev_mngr_handle_t dev_mngr)
{
    if (!dev_mngr || s_events) return ESP_ERR_INVALID_STATE;
    mosaico_hotplug_devices_init(dev_mngr);
    s_events = xQueueCreate(8, sizeof(mosaico_hotplug_event_t));
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "Failed to create event queue");

    ESP_RETURN_ON_ERROR(mosaico_module_mgr_init(NULL), TAG, "Failed to start module manager");

    mosaico_module_subscription_t subscription = {0};
    ESP_RETURN_ON_ERROR(mosaico_module_mgr_subscribe(on_module_event, NULL, &subscription), TAG,
        "Failed to subscribe to module manager");

    const bridge_dev_mngr_listener_t listener = {.attribute_changed = attribute_changed};
    bridge_dev_mngr_listener_id_t listener_id;
    ESP_RETURN_ON_ERROR(bridge_dev_mngr_add_listener(dev_mngr, &listener, &listener_id), TAG,
        "Failed to subscribe to native device writes");

    /* mosaico_module_mgr_subscribe() does not replay existing state. */
    for (int slot = MOSAICO_MODULE_MGR_SLOT_LEFT; slot < MOSAICO_MODULE_MGR_SLOT_COUNT; ++slot) {
        mosaico_module_mgr_info_t info = {0};
        if (mosaico_module_mgr_get_info((mosaico_module_mgr_slot_t)slot, &info) == ESP_OK) {
            enqueue_event(&info);
        }
    }

    uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#if defined(CONFIG_SPIRAM) && defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM)
    caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#endif
    ESP_RETURN_ON_FALSE(xTaskCreateWithCaps(bridge_task, "mosaico_hotplug", 6144, NULL, 5, NULL,
            caps) == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create hot-plug task");
    return ESP_OK;
}
