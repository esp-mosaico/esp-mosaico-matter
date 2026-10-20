/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include "esp_gsp.h"
#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "mosaic_runtime.h"
#include "thread_br_actions.h"
#include "thread_br_binds.h"
#include "thread_br_objects.h"
#if defined(ESP_PLATFORM)
#include "thread_border_router.h"
#endif

static void render(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
#if defined(ESP_PLATFORM)
    thread_border_router_status_t status;
    thread_border_router_get_status(&status);
    static const char *states[] = {
        "Unavailable", "Stopped", "Starting...", "Running", "Stopping...", "Thread error",
    };
    char text[100];
    esp_gsp_set_text(ui, GSP_BIND_THREAD_BOARD, status.board_present
                     ? "Thread-RCP detected / Right slot" : "Thread-RCP not detected");
    esp_gsp_set_text(ui, GSP_BIND_THREAD_STATE, states[status.state]);
    snprintf(text, sizeof(text), "Network: %s",
             status.network_name[0] ? status.network_name : "--");
    esp_gsp_set_text(ui, GSP_BIND_THREAD_NETWORK, text);
    snprintf(text, sizeof(text), "Channel: %u / Role: %s", status.channel,
             status.role[0] ? status.role : "disabled");
    esp_gsp_set_text(ui, GSP_BIND_THREAD_ROLE, text);
    esp_gsp_set_text(ui, GSP_BIND_THREAD_BACKBONE, status.backbone_connected
                     ? "Wi-Fi backbone connected" : "Wi-Fi backbone disconnected");
    const char *hint = "Press Start to enable Thread";
    if (status.state == THREAD_BR_ERROR) {
        snprintf(text, sizeof(text), "Start to retry (%s)", esp_err_to_name(status.last_error));
        hint = text;
    } else if (!status.enabled) {
        hint = "Auto-start disabled; press Start to enable";
    } else if (!status.board_present) {
        hint = "Waiting for Thread-RCP in the right slot";
    } else if (status.commissioning_ready) {
        hint = "Ready for Matter-over-Thread";
    } else if (status.state == THREAD_BR_RUNNING) {
        hint = status.backbone_connected ? "Waiting for Thread attachment" : "Connect hub Wi-Fi to commission";
    }
    esp_gsp_set_text(ui, GSP_BIND_THREAD_HINT, hint);
    bool busy = status.state == THREAD_BR_STARTING || status.state == THREAD_BR_STOPPING;
    esp_gsp_set_visible(ui, GSP_BIND_THREAD_START_VISIBLE,
                     !busy && (!status.enabled || status.state == THREAD_BR_ERROR));
    esp_gsp_set_visible(ui, GSP_BIND_THREAD_STOP_VISIBLE, !busy && status.enabled);
#else
    esp_gsp_set_visible(ui, GSP_BIND_THREAD_START_VISIBLE, false);
    esp_gsp_set_visible(ui, GSP_BIND_THREAD_STOP_VISIBLE, false);
#endif
}

static void started(esp_gsp_handle_t ui)
{
    render(ui, NULL);
    (void)esp_gsp_timer_create(ui, 250, render, NULL);
}

static void event(esp_gsp_handle_t ui, const mosaic_event_t *event)
{
#if defined(ESP_PLATFORM)
    if (!event || event->type != MOSAIC_EVENT_UI_CALL) { return; }
    thread_border_router_status_t status;
    thread_border_router_get_status(&status);
    if (status.state == THREAD_BR_STARTING || status.state == THREAD_BR_STOPPING) { return; }
    if (event->data.call.action_id == GSP_ACT_ID_THREAD_START) {
        (void)thread_border_router_set_enabled(true);
    } else if (event->data.call.action_id == GSP_ACT_ID_THREAD_STOP) {
        (void)thread_border_router_set_enabled(false);
    }
    render(ui, NULL);
#else
    (void)ui;
    (void)event;
#endif
}

static const mosaic_app_route_t routes[] = {
    {.action_id = GSP_ACT_ID_THREAD_MATTER, .target_name = "matter"},
};

const mosaic_app_descriptor_t mosaic_thread_br_app = {
    .id = 20,
    .launch_action = GSP_ACT_ID_APP_THREAD_BR,
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .name = "thread_br",
    .title = "Thread Border Router",
    .directory = &gsp_obj_directory_thread_br,
    .routes = routes,
    .route_count = sizeof(routes) / sizeof(routes[0]),
    .root_header_in_stack = true,
    .disable_swipe = true,
    .on_started = started,
    .on_event = event,
};
