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

/* The shared hub scene
 * (components/mosaico_ui/hub/scene/gen_scenes.py) has a
 * hardcoded 4-icon shortcut row (Matter/Thread/CLAW/Settings) that this
 * project does not control per-consumer -- editing it to add a dedicated,
 * correctly-labeled "Matter Bridge" tile is a product-design decision left
 * as follow-up (see examples/matter_bridge/README.md). Until then,
 * this app reuses the hub's existing "Matter" tile action id: since this
 * project's manifest never compiles the real (controller-facing) "matter"
 * app, there is no id collision, but the tile is mislabeled "Matter" and the
 * hub's "Thread" tile remains a dead tap target (its app isn't compiled
 * either). */
#include "mosaic_hub_actions.h"
#define GSP_ACT_ID_APP_MATTER_BRIDGE GSP_ACT_ID_APP_MATTER

#define QR_CANVAS_SIZE 200
#define QR_QUIET_MODULES 2
#define QR_PAYLOAD_MAX 200
#define MANUAL_CODE_MAX 32
#define DEVICE_ROWS 8

static bool s_commissioned;
static bool s_qr_ready;
static bool s_qr_on_network;
static bridge_core_handle_t s_core;
static bridge_dev_mngr_handle_t s_dev_mngr;
static uint16_t *s_qr_pixels; /* RGB565, QR_CANVAS_SIZE x QR_CANVAS_SIZE, allocated once */

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

/* Fill the fixed rows from the active entries in the shared bridge catalog.
 * Reading bridge_dev_mngr rather than the CLAW-owned model keeps custom profiles such
 * as the hot-plug camera visible while their endpoints are active. Retained
 * inactive entries (for example an unplugged camera) stay out of the UI. */
static void render_devices(esp_gsp_handle_t ui)
{
    static const uint16_t name_binds[DEVICE_ROWS] = {
        GSP_BIND_MATTER_BRIDGE_DEVICE_0_NAME, GSP_BIND_MATTER_BRIDGE_DEVICE_1_NAME,
        GSP_BIND_MATTER_BRIDGE_DEVICE_2_NAME, GSP_BIND_MATTER_BRIDGE_DEVICE_3_NAME,
        GSP_BIND_MATTER_BRIDGE_DEVICE_4_NAME, GSP_BIND_MATTER_BRIDGE_DEVICE_5_NAME,
        GSP_BIND_MATTER_BRIDGE_DEVICE_6_NAME, GSP_BIND_MATTER_BRIDGE_DEVICE_7_NAME,
    };
    static const uint16_t status_binds[DEVICE_ROWS] = {
        GSP_BIND_MATTER_BRIDGE_DEVICE_0_STATUS, GSP_BIND_MATTER_BRIDGE_DEVICE_1_STATUS,
        GSP_BIND_MATTER_BRIDGE_DEVICE_2_STATUS, GSP_BIND_MATTER_BRIDGE_DEVICE_3_STATUS,
        GSP_BIND_MATTER_BRIDGE_DEVICE_4_STATUS, GSP_BIND_MATTER_BRIDGE_DEVICE_5_STATUS,
        GSP_BIND_MATTER_BRIDGE_DEVICE_6_STATUS, GSP_BIND_MATTER_BRIDGE_DEVICE_7_STATUS,
    };

    size_t shown = 0;
    bridge_dev_mngr_device_info_t devices[BRIDGE_DEV_MNGR_MAX_DEVICES] = {0};
    size_t count = 0;
    if (s_dev_mngr) {
        esp_err_t error = bridge_dev_mngr_get_devices(s_dev_mngr, devices,
                                               BRIDGE_DEV_MNGR_MAX_DEVICES, &count);
        if (error == ESP_OK) {
            for (size_t index = 0;
                 index < count && shown < DEVICE_ROWS; ++index) {
                const bridge_dev_mngr_device_info_t *device = &devices[index];
                if (!device->active) {
                    continue;
                }
                const char *status = device->reachable ? "ready" : "unreachable";
                esp_gsp_set_text(ui, name_binds[shown],
                                 device->definition.name);
                esp_gsp_set_text(ui, status_binds[shown], status);
                ++shown;
            }
        }
    }
    for (size_t i = shown; i < DEVICE_ROWS; ++i) {
        esp_gsp_set_text(ui, name_binds[i], "");
        esp_gsp_set_text(ui, status_binds[i], "");
    }
}

/* Re-derives s_commissioned from the real commissioning state and syncs the
 * QR/status group visibility to it. Always called from started() (a fresh
 * esp_gsp_handle_t starts with the scene's authored default visibility --
 * QR group shown -- which must be corrected on re-entry if commissioning
 * already completed in an earlier visit); called from render() only while
 * not yet commissioned, so the 500ms poll stops touching visibility at all
 * once the one-way QR-to-status transition has happened. */
static void sync_commissioned_state(esp_gsp_handle_t ui)
{
    bool commissioned = false;
    if (s_core) (void)bridge_core_is_commissioned(s_core, &commissioned);
    s_commissioned = commissioned;
    esp_gsp_set_visible(ui, GSP_BIND_MATTER_BRIDGE_QR_GROUP_VISIBLE, !s_commissioned);
    esp_gsp_set_visible(ui, GSP_BIND_MATTER_BRIDGE_STATUS_GROUP_VISIBLE, s_commissioned);
}

static void render(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (!s_commissioned) {
        sync_commissioned_state(ui);
        if (!s_commissioned) {
            render_pairing(ui);
            return;
        }
    }

    render_devices(ui);
}

static void started(esp_gsp_handle_t ui)
{
    s_qr_ready = false;
    esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_MANUAL_CODE, "");
    sync_commissioned_state(ui);
    render(ui, NULL);
    (void)esp_gsp_timer_create(ui, 500, render, NULL);
}

static void event(esp_gsp_handle_t ui, const mosaic_event_t *evt)
{
    (void)ui;
    (void)evt;
    /* Nothing to do here: the "< Home" button's CALL event (action id
     * GSP_ACT_ID_BACK_HUB) is matched against `routes` below by
     * mosaic_runtime's generic event router, not handled by this App. */
}

static const mosaic_app_route_t routes[] = {
    {.action_id = GSP_ACT_ID_BACK_HUB, .target_name = "mosaic-hub"},
};

const mosaic_app_descriptor_t mosaic_matter_bridge_app = {
    .id = 21,
    .launch_action = GSP_ACT_ID_APP_MATTER_BRIDGE,
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .routes = routes,
    .route_count = sizeof(routes) / sizeof(routes[0]),
    .name = "matter_bridge",
    .title = "Matter Bridge",
    .directory = &gsp_obj_directory_matter_bridge,
    .custom_shell = true,
    .disable_swipe = true,
    .on_started = started,
    .on_event = event,
};
