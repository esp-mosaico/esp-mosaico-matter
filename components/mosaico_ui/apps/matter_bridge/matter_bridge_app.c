/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "esp_gsp.h"
#include "mosaic_app_catalog.h"
#include "mosaic_qr_canvas.h"
#include "mosaic_runtime.h"

#include "bridge_core.h"
#include "bridge_dev_mngr.h"
#include "mosaic_matter_bridge.h"
#include "mosaic_settings.h"

#include "matter_bridge_actions.h"
#include "matter_bridge_binds.h"
#include "matter_bridge_objects.h"
#include "matter_bridge_detail.h"

/* The bridge uses the shared launcher's Matter shortcut. Its app roster
 * excludes the controller app, so the action has a single owner. */
#include "mosaic_hub_actions.h"
#define GSP_ACT_ID_APP_MATTER_BRIDGE GSP_ACT_ID_APP_MATTER

#define QR_CANVAS_SIZE 200
#define QR_QUIET_MODULES 2
#define QR_PAYLOAD_MAX 200
#define MANUAL_CODE_MAX 32
#define PAGE_SIZE 4U

static bool s_commissioned;
static bool s_qr_ready;
static bool s_qr_on_network;
static bridge_core_handle_t s_core;
static bridge_dev_mngr_handle_t s_dev_mngr;
static uint16_t *s_qr_pixels; /* RGB565, QR_CANVAS_SIZE x QR_CANVAS_SIZE, allocated once */
static bridge_dev_mngr_device_info_t s_devices[BRIDGE_DEV_MNGR_MAX_DEVICES];
static size_t s_device_count;
static size_t s_page;
static bool s_detail_visible;

void mosaic_matter_bridge_set_handles(bridge_core_handle_t core,
                                      bridge_dev_mngr_handle_t dev_mngr)
{
    s_core = core;
    s_dev_mngr = dev_mngr;
}

/* Reopen discovery on entry and when connectivity changes: BLE before Wi-Fi,
 * then DNS-SD-only discovery once the bridge has an IP address. */
static void render_pairing(esp_gsp_handle_t ui)
{
    mosaic_settings_network_t network = {0};
    bool on_network = mosaic_settings_get_wifi(&network) == ESP_OK && network.connected;
    if (s_qr_ready && s_qr_on_network == on_network) return;
    s_qr_ready = false;
    gsp_matter_bridge_matter_bridge_qr_canvas_set_visible(ui, false);
    esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_MANUAL_CODE, "");
    if (!s_core) {
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_PAIRING_HINT, "Waiting for pairing window");
        return;
    }
    esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_PAIRING_HINT, "Preparing pairing code");
    char payload[QR_PAYLOAD_MAX] = {0};
    if (bridge_core_get_qr_code(s_core, payload, sizeof(payload)) != ESP_OK) return;
    if (!mosaic_qr_render_to_canvas(ui, GSP_BIND_MATTER_BRIDGE_QR_CANVAS, payload, qrcodegen_Ecc_MEDIUM,
            QR_QUIET_MODULES, QR_CANVAS_SIZE, &s_qr_pixels)) return;
    esp_err_t error = on_network ? bridge_core_open_on_network_commissioning_window(s_core) :
                                  bridge_core_open_commissioning_window(s_core);
    if (error != ESP_OK) {
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_PAIRING_HINT, "Waiting for pairing window");
        return;
    }
    char manual_code[MANUAL_CODE_MAX] = {0};
    if (bridge_core_get_manual_pairing_code(s_core, manual_code, sizeof(manual_code)) == ESP_OK && manual_code[0]) {
        char text[64];
        snprintf(text, sizeof(text), "Manual code: %s", manual_code);
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_MANUAL_CODE, text);
    }
    esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_PAIRING_HINT,
                     on_network ? "Scan to pair on this network" : "Scan to pair over Bluetooth");
    if (gsp_matter_bridge_matter_bridge_qr_canvas_set_visible(ui, true) != ESP_GSP_OK) return;
    s_qr_on_network = on_network;
    s_qr_ready = true;
}

/* Use the shared catalog so native accessories and CLAW devices appear alike. */
static void render_devices(esp_gsp_handle_t ui)
{
    static const uint16_t name_binds[PAGE_SIZE] = {
        GSP_BIND_MATTER_BRIDGE_DEVICE_0_NAME, GSP_BIND_MATTER_BRIDGE_DEVICE_1_NAME,
        GSP_BIND_MATTER_BRIDGE_DEVICE_2_NAME, GSP_BIND_MATTER_BRIDGE_DEVICE_3_NAME,
    };
    static const uint16_t status_binds[PAGE_SIZE] = {
        GSP_BIND_MATTER_BRIDGE_DEVICE_0_STATUS, GSP_BIND_MATTER_BRIDGE_DEVICE_1_STATUS,
        GSP_BIND_MATTER_BRIDGE_DEVICE_2_STATUS, GSP_BIND_MATTER_BRIDGE_DEVICE_3_STATUS,
    };
    static const uint16_t visible_binds[PAGE_SIZE] = {
        GSP_BIND_MATTER_BRIDGE_DEVICE_0_VISIBLE, GSP_BIND_MATTER_BRIDGE_DEVICE_1_VISIBLE,
        GSP_BIND_MATTER_BRIDGE_DEVICE_2_VISIBLE, GSP_BIND_MATTER_BRIDGE_DEVICE_3_VISIBLE,
    };
    static const uint16_t hint_binds[PAGE_SIZE] = {
        GSP_BIND_MATTER_BRIDGE_DEVICE_0_HINT, GSP_BIND_MATTER_BRIDGE_DEVICE_1_HINT,
        GSP_BIND_MATTER_BRIDGE_DEVICE_2_HINT, GSP_BIND_MATTER_BRIDGE_DEVICE_3_HINT,
    };
    size_t count = 0;
    if (s_dev_mngr) {
        esp_err_t error = bridge_dev_mngr_get_devices(s_dev_mngr, s_devices,
                                               BRIDGE_DEV_MNGR_MAX_DEVICES, &count);
        if (error != ESP_OK) {
            esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_DEVICE_COUNT, "Unable to load devices");
            return;
        }
    }
    s_device_count = 0;
    for (size_t index = 0; index < count; ++index) {
        if (s_devices[index].active) s_devices[s_device_count++] = s_devices[index];
    }
    size_t pages = s_device_count ? (s_device_count + PAGE_SIZE - 1) / PAGE_SIZE : 1;
    if (s_page >= pages) s_page = pages - 1;
    char text[96];
    snprintf(text, sizeof(text), "%u device%s", (unsigned)s_device_count,
             s_device_count == 1 ? "" : "s");
    esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_DEVICE_COUNT, text);
    snprintf(text, sizeof(text), "%u / %u", (unsigned)(s_page + 1), (unsigned)pages);
    esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_PAGE, text);
    for (size_t slot = 0; slot < PAGE_SIZE; ++slot) {
        size_t index = s_page * PAGE_SIZE + slot;
        esp_gsp_set_visible(ui, visible_binds[slot], index < s_device_count);
        if (index >= s_device_count) continue;
        const bridge_dev_mngr_device_info_t *device = &s_devices[index];
        esp_gsp_set_text(ui, name_binds[slot], device->definition.name);
        const char *type = matter_bridge_device_is_camera(device) ? "Camera" :
            !strcmp(device->definition.profile_id, BRIDGE_DEV_MNGR_PROFILE_OUTLET) ? "Outlet" :
            matter_bridge_device_has_controls(device) ? "Light" : "Sensor";
        snprintf(text, sizeof(text), "%s | %s", type,
                 device->reachable ? "Online" : "Offline");
        esp_gsp_set_text(ui, status_binds[slot], text);
        esp_gsp_set_text(ui, hint_binds[slot], matter_bridge_device_is_camera(device) ? "" :
                         matter_bridge_device_has_controls(device) ? "Tap for controls" : "Tap for details");
    }
}

/* Recheck on entry and until commissioning completes. Wi-Fi connectivity
 * alone must never replace the Matter pairing QR with the device page. */
static void sync_commissioned_state(void)
{
    bool commissioned = false;
    if (s_core) (void)bridge_core_is_commissioned(s_core, &commissioned);
    s_commissioned = commissioned;
}

static void sync_view(esp_gsp_handle_t ui)
{
    const gsp_component_update_t updates[] = {
        {GSP_OBJ_KEY_MATTER_BRIDGE_QR_GROUP, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = !s_commissioned}},
        {GSP_OBJ_KEY_MATTER_BRIDGE_STATUS_GROUP, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = s_commissioned && !s_detail_visible}},
        {GSP_OBJ_KEY_MATTER_BRIDGE_DETAIL, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = s_commissioned && s_detail_visible}},
    };
    (void)esp_gsp_component_set_many(ui, updates, sizeof(updates) / sizeof(updates[0]));
}

static void render(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (!s_commissioned) {
        sync_commissioned_state();
        if (!s_commissioned) {
            sync_view(ui);
            render_pairing(ui);
            return;
        }
    }

    if (s_detail_visible && !matter_bridge_detail_tick(ui)) {
        s_detail_visible = false;
    }
    if (!s_detail_visible) {
        // A full render queue must not reveal Home while old list rows remain.
        if (!matter_bridge_detail_close(ui)) return;
        render_devices(ui);
    }
    sync_view(ui);
}

static void started(esp_gsp_handle_t ui)
{
    s_qr_ready = false;
    s_page = 0;
    s_device_count = 0;
    s_detail_visible = false;
    matter_bridge_detail_init(s_dev_mngr);
    esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_MANUAL_CODE, "");
    sync_commissioned_state();
    render(ui, NULL);
}

static void stopped(esp_gsp_handle_t ui)
{
    (void)matter_bridge_detail_close(ui);
}

static bool back(esp_gsp_handle_t ui, int64_t timestamp_us)
{
    (void)timestamp_us;
    if (!s_detail_visible) return false;
    if (!matter_bridge_detail_back()) {
        s_detail_visible = false;
    }
    render(ui, NULL);
    return true;
}

static void event(esp_gsp_handle_t ui, const mosaic_event_t *evt)
{
    if (evt->type == MOSAIC_EVENT_POINTER) {
        matter_bridge_detail_pointer(evt->data.pointer.pressed);
        return;
    }
    if (evt->type == MOSAIC_EVENT_TIMER) {
        render(ui, NULL);
        return;
    }
    if (evt->type != MOSAIC_EVENT_UI_CALL || !s_commissioned) return;
    uint16_t action = evt->data.call.action_id;
    if (s_detail_visible) {
        if (action == GSP_ACT_ID_MATTER_BRIDGE_DETAIL_BACK) {
            (void)back(ui, evt->timestamp_us);
            return;
        }
        if (action == GSP_ACT_ID_MATTER_BRIDGE_ENTRY_OPEN) {
            matter_bridge_detail_open_entry(evt->data.call.item);
        } else {
            matter_bridge_detail_control(action, (int32_t)evt->data.call.arg);
        }
    } else if (action == GSP_ACT_ID_MATTER_BRIDGE_PREVIOUS) {
        if (s_page) --s_page;
    } else if (action == GSP_ACT_ID_MATTER_BRIDGE_NEXT) {
        if ((s_page + 1) * PAGE_SIZE < s_device_count) ++s_page;
    } else {
        const uint16_t actions[PAGE_SIZE] = {
            GSP_ACT_ID_MATTER_BRIDGE_DEVICE_0, GSP_ACT_ID_MATTER_BRIDGE_DEVICE_1,
            GSP_ACT_ID_MATTER_BRIDGE_DEVICE_2, GSP_ACT_ID_MATTER_BRIDGE_DEVICE_3,
        };
        for (size_t slot = 0; slot < PAGE_SIZE; ++slot) {
            size_t index = s_page * PAGE_SIZE + slot;
            if (action != actions[slot] || index >= s_device_count) continue;
            bridge_dev_mngr_device_info_t current;
            if (bridge_dev_mngr_get_device(s_dev_mngr, s_devices[index].definition.id, &current) == ESP_OK &&
                current.active && !matter_bridge_device_is_camera(&current)) {
                s_detail_visible = matter_bridge_detail_open(ui, &current);
            }
            break;
        }
    }
    render(ui, NULL);
}

const mosaic_app_descriptor_t mosaic_matter_bridge_app = {
    .id = 21,
    .launch_action = GSP_ACT_ID_APP_MATTER_BRIDGE,
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .back_exits_app = true,
    .root_header_in_stack = true,
    .name = "matter_bridge",
    .title = "Matter Bridge",
    .directory = &gsp_obj_directory_matter_bridge,
    .instance_slots = 12,
    .disable_swipe = true,
    .on_started = started,
    .on_stopping = stopped,
    .on_event = event,
    .on_back = back,
};
