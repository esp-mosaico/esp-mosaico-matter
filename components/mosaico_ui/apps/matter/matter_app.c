/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_matter.h"

#include "matter_commissioning.h"
#include "matter_detail.h"
#include "matter_home.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_gsp.h"
#include "matter_actions.h"
#include "matter_binds.h"
#include "matter_objects.h"
#include "matter_templates.h"
#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "mosaic_loader.h"
#include "mosaic_runtime.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#endif

#define MATTER_MANUAL_CODE_MAX 22U
#define MATTER_SETUP_CODE_MAX 256U

typedef enum {
    MATTER_VIEW_DASHBOARD = 0,
    MATTER_VIEW_SCAN,
    MATTER_VIEW_MANUAL,
    MATTER_VIEW_PROGRESS,
    MATTER_VIEW_DETAIL,
} matter_view_t;

typedef struct {
    fabric_core_handle_t core;
    fabric_manager_handle_t manager;
    fabric_manager_listener_id_t listener_id;
    esp_gsp_handle_t ui;
    matter_view_t view;
    bool view_pending;
    char manual_code[MATTER_MANUAL_CODE_MAX];
    bool refresh_pending;
#if defined(ESP_PLATFORM)
    portMUX_TYPE event_lock;
#endif
} matter_app_state_t;

static matter_app_state_t s_app = {
    .view = MATTER_VIEW_DASHBOARD,
#if defined(ESP_PLATFORM)
    .event_lock = portMUX_INITIALIZER_UNLOCKED,
#endif
};

extern const mosaic_app_descriptor_t mosaic_matter_app;

void mosaic_matter_set_local_fabric(fabric_core_handle_t core,
                                    fabric_manager_handle_t manager)
{
    s_app.core = core;
    s_app.manager = manager;
}

static void set_view(esp_gsp_handle_t ui, matter_view_t view)
{
    s_app.view = view;
    const gsp_component_update_t updates[] = {
        {GSP_OBJ_KEY_MATTER_DASHBOARD, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = view == MATTER_VIEW_DASHBOARD}},
        {GSP_OBJ_KEY_MATTER_SCAN, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = view == MATTER_VIEW_SCAN}},
        {GSP_OBJ_KEY_MATTER_MANUAL, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = view == MATTER_VIEW_MANUAL}},
        {GSP_OBJ_KEY_MATTER_PROGRESS, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = view == MATTER_VIEW_PROGRESS}},
        {GSP_OBJ_KEY_MATTER_DETAIL, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = view == MATTER_VIEW_DETAIL}},
    };
    const esp_gsp_err_t error = esp_gsp_component_set_many(
        ui, updates, sizeof(updates) / sizeof(updates[0]));
#if defined(ESP_PLATFORM)
    if (error != ESP_GSP_OK && !s_app.view_pending) {
        ESP_LOGW("matter_app", "set view %u failed: 0x%x",
                 (unsigned)view, (unsigned)error);
    }
#endif
    s_app.view_pending = error != ESP_GSP_OK;
}

static void mark_refresh(void)
{
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_app.event_lock);
#endif
    s_app.refresh_pending = true;
#if defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&s_app.event_lock);
#endif
}

static bool take_refresh(void)
{
    bool refresh = false;
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_app.event_lock);
#endif
    refresh = s_app.refresh_pending;
    s_app.refresh_pending = false;
#if defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&s_app.event_lock);
#endif
    return refresh;
}

static void device_set_changed(fabric_manager_handle_t manager, void *user_ctx)
{
    (void)manager;
    (void)user_ctx;
    mark_refresh();
}

static void properties_changed(fabric_manager_handle_t manager,
                               const fabric_manager_property_change_t *changes,
                               size_t change_count, void *user_ctx)
{
    (void)manager;
    (void)changes;
    (void)change_count;
    (void)user_ctx;
    matter_detail_property_changed();
    mark_refresh();
}

static void show_dashboard(esp_gsp_handle_t ui)
{
    matter_commissioning_stop_scan(ui);
    matter_detail_close(ui);
    set_view(ui, MATTER_VIEW_DASHBOARD);
    (void)matter_home_refresh(ui);
}

static void matter_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
#if defined(ESP_PLATFORM)
    if (s_app.view_pending) {
        set_view(ui, s_app.view);
        if (s_app.view_pending) return;
    }
    if (take_refresh()) {
        if (s_app.view == MATTER_VIEW_DETAIL) {
            const esp_err_t error = matter_detail_refresh(ui);
            if (error == ESP_ERR_NOT_FOUND) {
                show_dashboard(ui);
            } else if (error != ESP_OK) {
                if (error == ESP_ERR_TIMEOUT) mark_refresh();
                else ESP_LOGW("matter_app", "detail refresh failed: %s",
                              esp_err_to_name(error));
            }
        } else {
            (void)matter_home_refresh(ui);
        }
    }
    if (s_app.view == MATTER_VIEW_DETAIL) {
        matter_detail_process_events(ui);
    }
    if (matter_commissioning_process_events(ui)) {
        show_dashboard(ui);
    }

    char scanned_code[MATTER_SETUP_CODE_MAX] = {};
    if (s_app.view == MATTER_VIEW_SCAN &&
        matter_commissioning_take_scanned_code(scanned_code,
                                               sizeof(scanned_code))) {
        set_view(ui, MATTER_VIEW_PROGRESS);
        matter_commissioning_begin(ui, scanned_code);
    }
#endif
}

static void matter_started(esp_gsp_handle_t ui)
{
#if defined(ESP_PLATFORM)
    ESP_LOGI("matter_app",
             "started: core=%p manager=%p internal_free=%u spiram_free=%u",
             s_app.core, s_app.manager,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
    if (s_app.manager && s_app.listener_id) {
        (void)fabric_manager_remove_listener(s_app.manager, s_app.listener_id);
    }
    s_app.listener_id = 0;
    s_app.ui = ui;
    s_app.view = MATTER_VIEW_DASHBOARD;
    s_app.view_pending = false;
    s_app.manual_code[0] = '\0';
    s_app.refresh_pending = false;

    matter_home_init(s_app.manager);
    matter_detail_init(s_app.manager);
    matter_commissioning_init(s_app.core);
    matter_commissioning_reset();

    if (s_app.manager) {
        const fabric_manager_listener_t listener = {
            .device_set_changed = device_set_changed,
            .properties_changed = properties_changed,
            .user_ctx = NULL,
        };
        if (fabric_manager_add_listener(s_app.manager, &listener,
                                 &s_app.listener_id) != ESP_OK) {
            s_app.listener_id = 0;
        }
    }

    set_view(ui, MATTER_VIEW_DASHBOARD);
    if (!s_app.manager) {
        esp_gsp_set_text(ui, GSP_BIND_MATTER_DEVICE_COUNT,
                         "Matter controller unavailable");
    } else {
        (void)matter_home_refresh(ui);
    }
}

static void matter_stopped(esp_gsp_handle_t ui)
{
    matter_commissioning_stop_scan(ui);
    if (s_app.manager && s_app.listener_id) {
        (void)fabric_manager_remove_listener(s_app.manager, s_app.listener_id);
    }
    s_app.listener_id = 0;
    matter_detail_close(ui);
    matter_home_reset();
    s_app.ui = NULL;
    s_app.view_pending = false;
}

static void render_manual_code(esp_gsp_handle_t ui)
{
    esp_gsp_set_text(ui, GSP_BIND_MATTER_MANUAL_CODE,
                     s_app.manual_code[0] ? s_app.manual_code
                                          : "Enter setup code");
}

static void append_manual_digit(esp_gsp_handle_t ui, unsigned digit)
{
    const size_t length = strlen(s_app.manual_code);
    if (digit <= 9 && length + 1 < sizeof(s_app.manual_code)) {
        s_app.manual_code[length] = (char)('0' + digit);
        s_app.manual_code[length + 1] = '\0';
    }
    render_manual_code(ui);
}

static void backspace_manual_code(esp_gsp_handle_t ui)
{
    const size_t length = strlen(s_app.manual_code);
    if (length > 0) s_app.manual_code[length - 1] = '\0';
    render_manual_code(ui);
}

static void open_device(esp_gsp_handle_t ui, size_t slot)
{
    fabric_manager_device_id_t device_id = 0;
    if (matter_home_device_at_slot(slot, &device_id) != ESP_OK) return;
    const esp_err_t error = matter_detail_open(ui, device_id);
    if (error == ESP_OK || error == ESP_ERR_TIMEOUT) {
        set_view(ui, MATTER_VIEW_DETAIL);
        if (error == ESP_ERR_TIMEOUT) mark_refresh();
    } else {
#if defined(ESP_PLATFORM)
        ESP_LOGW("matter_app", "open device failed: %s", esp_err_to_name(error));
#endif
    }
}

static bool handle_call(esp_gsp_handle_t ui,
                        const gsp_matter_call_event_t *call)
{
    switch (call->action) {
    case GSP_MATTER_ACTION_MATTER_ADD:
        set_view(ui, MATTER_VIEW_SCAN);
        matter_commissioning_start_scan(ui);
        return true;
    case GSP_MATTER_ACTION_MATTER_SCAN_CANCEL:
        show_dashboard(ui);
        return true;
    case GSP_MATTER_ACTION_MATTER_MANUAL:
        matter_commissioning_stop_scan(ui);
        s_app.manual_code[0] = '\0';
        render_manual_code(ui);
        set_view(ui, MATTER_VIEW_MANUAL);
        return true;
    case GSP_MATTER_ACTION_MATTER_MANUAL_CANCEL:
        show_dashboard(ui);
        return true;
    case GSP_MATTER_ACTION_MATTER_DELETE:
        backspace_manual_code(ui);
        return true;
    case GSP_MATTER_ACTION_MATTER_PAIR: {
        const size_t length = strlen(s_app.manual_code);
        if (length != 11 && length != 21) {
            esp_gsp_set_text(ui, GSP_BIND_MATTER_MANUAL_HINT,
                             "Use an 11 or 21 digit setup code.");
            return true;
        }
        set_view(ui, MATTER_VIEW_PROGRESS);
        matter_commissioning_begin(ui, s_app.manual_code);
        return true;
    }
    case GSP_MATTER_ACTION_MATTER_COMMISSION_CANCEL:
        if (s_app.core) (void)fabric_core_cancel_commissioning(s_app.core);
        show_dashboard(ui);
        return true;
    case GSP_MATTER_ACTION_MATTER_PREVIOUS:
        matter_home_previous_page(ui);
        return true;
    case GSP_MATTER_ACTION_MATTER_NEXT:
        matter_home_next_page(ui);
        return true;
    case GSP_MATTER_ACTION_MATTER_ROOM_ALL:
        matter_home_select_all(ui);
        return true;
    case GSP_MATTER_ACTION_MATTER_ROOM_PREVIOUS:
        matter_home_previous_rooms(ui);
        return true;
    case GSP_MATTER_ACTION_MATTER_ROOM_NEXT:
        matter_home_next_rooms(ui);
        return true;
    case GSP_MATTER_ACTION_MATTER_ROOM_0:
    case GSP_MATTER_ACTION_MATTER_ROOM_1:
    case GSP_MATTER_ACTION_MATTER_ROOM_2:
        matter_home_select_room(ui,
                                call->action - GSP_MATTER_ACTION_MATTER_ROOM_0);
        return true;
    case GSP_MATTER_ACTION_MATTER_DEVICE_0:
    case GSP_MATTER_ACTION_MATTER_DEVICE_1:
    case GSP_MATTER_ACTION_MATTER_DEVICE_2:
    case GSP_MATTER_ACTION_MATTER_DEVICE_3:
        open_device(ui,
                    call->action - GSP_MATTER_ACTION_MATTER_DEVICE_0);
        return true;
    case GSP_MATTER_ACTION_MATTER_DETAIL_BACK:
        if (!matter_detail_back(ui)) show_dashboard(ui);
        else mark_refresh();
        return true;
    case GSP_MATTER_ACTION_MATTER_ENTRY_OPEN:
        if (matter_detail_open_entry(ui, call->item) == ESP_ERR_TIMEOUT)
            mark_refresh();
        return true;
    case GSP_MATTER_ACTION_MATTER_MODE_FIRST_PREVIOUS:
    case GSP_MATTER_ACTION_MATTER_MODE_FIRST_NEXT:
    case GSP_MATTER_ACTION_MATTER_MODE_SECOND_PREVIOUS:
    case GSP_MATTER_ACTION_MATTER_MODE_SECOND_NEXT:
        matter_detail_step_mode(ui,
            call->action == GSP_MATTER_ACTION_MATTER_MODE_SECOND_PREVIOUS ||
            call->action == GSP_MATTER_ACTION_MATTER_MODE_SECOND_NEXT,
            call->action == GSP_MATTER_ACTION_MATTER_MODE_FIRST_PREVIOUS ||
            call->action == GSP_MATTER_ACTION_MATTER_MODE_SECOND_PREVIOUS ? -1 : 1);
        return true;
    case GSP_MATTER_ACTION_MATTER_TARGET_SET:
        matter_detail_set_target(ui, (int32_t)call->arg);
        return true;
    case GSP_MATTER_ACTION_MATTER_SPEED_SET:
        matter_detail_set_speed(ui, (int32_t)call->arg);
        return true;
    case GSP_MATTER_ACTION_MATTER_PAUSE_RESUME:
        matter_detail_pause_resume(ui);
        return true;
    case GSP_MATTER_ACTION_MATTER_GO_HOME:
        matter_detail_go_home(ui);
        return true;
    case GSP_MATTER_ACTION_MATTER_LEVEL:
        matter_detail_set_level(ui, (int32_t)call->arg);
        return true;
    case GSP_MATTER_ACTION_MATTER_HUE:
        matter_detail_set_hue(ui, (int32_t)call->arg);
        return true;
    case GSP_MATTER_ACTION_MATTER_SATURATION:
        matter_detail_set_saturation(ui, (int32_t)call->arg);
        return true;
    case GSP_MATTER_ACTION_MATTER_COLOR_TEMP:
        matter_detail_set_color_temperature(ui, (int32_t)call->arg);
        return true;
    case GSP_MATTER_ACTION_MATTER_POWER_TOGGLE:
        matter_detail_toggle_power(ui);
        return true;
    default:
        if (call->action >= GSP_MATTER_ACTION_MATTER_DIGIT_0 &&
            call->action <= GSP_MATTER_ACTION_MATTER_DIGIT_9) {
            append_manual_digit(
                ui, call->action - GSP_MATTER_ACTION_MATTER_DIGIT_0);
            return true;
        }
        return false;
    }
}

static bool matter_back(esp_gsp_handle_t ui, int64_t timestamp_us)
{
    (void)timestamp_us;
    if (s_app.view == MATTER_VIEW_DASHBOARD) return false;
    if (s_app.view == MATTER_VIEW_DETAIL && matter_detail_back(ui)) {
        mark_refresh();
        return true;
    }
    show_dashboard(ui);
    return true;
}

static void matter_event(esp_gsp_handle_t ui,
                         const struct mosaic_event *raw_event)
{
    const mosaic_event_t *event = raw_event;
    if (!event) return;
    switch (event->type) {
    case MOSAIC_EVENT_START:
        matter_started(ui);
        break;
    case MOSAIC_EVENT_STOP:
        matter_stopped(ui);
        break;
    case MOSAIC_EVENT_UI_CALL: {
        gsp_matter_call_event_t call;
        const esp_gsp_event_t gsp_event = {
            .type = ESP_GSP_EVENT_CALL,
            .scene_id = event->data.call.scene_id,
            .action_id = event->data.call.action_id,
            .arg = event->data.call.arg,
            .list = event->data.call.list,
            .item = event->data.call.item,
        };
        if (gsp_matter_event_decode_call(&gsp_event, &call)) {
            (void)handle_call(ui, &call);
        }
        break;
    }
    case MOSAIC_EVENT_TIMER:
        matter_tick(ui, NULL);
        break;
    default:
        break;
    }
}

const mosaic_app_descriptor_t mosaic_matter_app = {
    .id = 7,
    .launch_action = GSP_ACT_ID_APP_MATTER,
    /* The on-screen detail Back is local navigation, not an App exit route. */
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .back_exits_app = true,
    .root_header_in_stack = true,
    .name = "matter",
    .title = "Matter",
    .directory = &gsp_obj_directory_matter,
    .instance_slots = 12,
    /* Startup images plus a 480x480 RGB565 preview must fit even after
     * the camera pool has reserved its PSRAM at boot. */
    .image_cache_bytes = 1024U * 1024U,
    .disable_swipe = true,
    .on_event = matter_event,
    .on_back = matter_back,
};
