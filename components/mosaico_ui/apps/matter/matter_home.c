/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "matter_home.h"

#include "matter_view_model.h"

#include "sdkconfig.h"
#include "matter_binds.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MATTER_PAGE_SIZE 4U
#define MATTER_ROOM_SLOT_COUNT 3U
#define MATTER_COLOR_ACTIVE UINT32_C(0xFA40)
#define MATTER_COLOR_INACTIVE UINT32_C(0x2124)

typedef struct {
    char room[FABRIC_MANAGER_ROOM_MAX];
    bool unassigned;
} matter_room_option_t;

typedef struct {
    fabric_manager_handle_t manager;
    matter_vm_device_t devices[CONFIG_FABRIC_MANAGER_MAX_DEVICES];
    size_t device_count;
    size_t visible_count;
    size_t visible_indices[CONFIG_FABRIC_MANAGER_MAX_DEVICES];
    matter_room_option_t room_options[CONFIG_FABRIC_MANAGER_MAX_DEVICES + 1U];
    size_t room_count;
    size_t room_window;
    bool filter_all;
    bool filter_unassigned;
    char filter_room[FABRIC_MANAGER_ROOM_MAX];
    size_t page;
} matter_home_state_t;

static matter_home_state_t s_home;

static bool room_exists(const char *room)
{
    for (size_t index = 0; index < s_home.room_count; ++index) {
        if (!s_home.room_options[index].unassigned &&
            !strcmp(s_home.room_options[index].room, room)) {
            return true;
        }
    }
    return false;
}

static void build_rooms(void)
{
    s_home.room_count = 0;
    for (size_t index = 0; index < s_home.device_count; ++index) {
        const char *room = s_home.devices[index].room;
        if (room[0] && !room_exists(room)) {
            snprintf(s_home.room_options[s_home.room_count].room,
                     sizeof(s_home.room_options[s_home.room_count].room), "%s",
                     room);
            ++s_home.room_count;
        }
    }
    bool has_unassigned = false;
    for (size_t index = 0; index < s_home.device_count; ++index) {
        has_unassigned |= s_home.devices[index].room[0] == '\0';
    }
    if (has_unassigned) {
        s_home.room_options[s_home.room_count++].unassigned = true;
    }
    if (s_home.room_window >= s_home.room_count) {
        s_home.room_window = s_home.room_count > MATTER_ROOM_SLOT_COUNT
                                 ? s_home.room_count - MATTER_ROOM_SLOT_COUNT
                                 : 0;
    }
}

static bool matches_filter(const matter_vm_device_t *device)
{
    if (s_home.filter_all) return true;
    if (s_home.filter_unassigned) return device->room[0] == '\0';
    return !strcmp(device->room, s_home.filter_room);
}

static void rebuild_visible(void)
{
    s_home.visible_count = 0;
    for (size_t index = 0; index < s_home.device_count; ++index) {
        if (matches_filter(&s_home.devices[index])) {
            s_home.visible_indices[s_home.visible_count++] = index;
        }
    }
    const size_t page_count =
        (s_home.visible_count + MATTER_PAGE_SIZE - 1U) / MATTER_PAGE_SIZE;
    if (page_count == 0) {
        s_home.page = 0;
    } else if (s_home.page >= page_count) {
        s_home.page = page_count - 1U;
    }
}

static void render_rooms(esp_gsp_handle_t ui)
{
    static const uint16_t visible_binds[MATTER_ROOM_SLOT_COUNT] = {
        GSP_BIND_MATTER_ROOM_0_VISIBLE,
        GSP_BIND_MATTER_ROOM_1_VISIBLE,
        GSP_BIND_MATTER_ROOM_2_VISIBLE,
    };
    static const uint16_t text_binds[MATTER_ROOM_SLOT_COUNT] = {
        GSP_BIND_MATTER_ROOM_0_TEXT,
        GSP_BIND_MATTER_ROOM_1_TEXT,
        GSP_BIND_MATTER_ROOM_2_TEXT,
    };
    static const uint16_t color_binds[MATTER_ROOM_SLOT_COUNT] = {
        GSP_BIND_MATTER_ROOM_0_COLOR,
        GSP_BIND_MATTER_ROOM_1_COLOR,
        GSP_BIND_MATTER_ROOM_2_COLOR,
    };
    (void)esp_gsp_set_color(ui, GSP_BIND_MATTER_ROOM_ALL_COLOR,
                            s_home.filter_all ? MATTER_COLOR_ACTIVE
                                              : MATTER_COLOR_INACTIVE);
    for (size_t slot = 0; slot < MATTER_ROOM_SLOT_COUNT; ++slot) {
        const size_t index = s_home.room_window + slot;
        const bool visible = index < s_home.room_count;
        (void)esp_gsp_set_visible(ui, visible_binds[slot], visible);
        if (!visible) continue;
        const matter_room_option_t *option = &s_home.room_options[index];
        const bool active = option->unassigned
                                ? s_home.filter_unassigned
                                : (!s_home.filter_all &&
                                   !strcmp(s_home.filter_room, option->room));
        (void)esp_gsp_set_text(ui, text_binds[slot],
                               option->unassigned ? "Other" : option->room);
        (void)esp_gsp_set_color(ui, color_binds[slot],
                               active ? MATTER_COLOR_ACTIVE
                                      : MATTER_COLOR_INACTIVE);
    }
}

static void render_devices(esp_gsp_handle_t ui)
{
    static const uint16_t name_binds[MATTER_PAGE_SIZE] = {
        GSP_BIND_MATTER_DEVICE_0_NAME,
        GSP_BIND_MATTER_DEVICE_1_NAME,
        GSP_BIND_MATTER_DEVICE_2_NAME,
        GSP_BIND_MATTER_DEVICE_3_NAME,
    };
    static const uint16_t status_binds[MATTER_PAGE_SIZE] = {
        GSP_BIND_MATTER_DEVICE_0_STATUS,
        GSP_BIND_MATTER_DEVICE_1_STATUS,
        GSP_BIND_MATTER_DEVICE_2_STATUS,
        GSP_BIND_MATTER_DEVICE_3_STATUS,
    };
    static const uint16_t visible_binds[MATTER_PAGE_SIZE] = {
        GSP_BIND_MATTER_DEVICE_0_VISIBLE,
        GSP_BIND_MATTER_DEVICE_1_VISIBLE,
        GSP_BIND_MATTER_DEVICE_2_VISIBLE,
        GSP_BIND_MATTER_DEVICE_3_VISIBLE,
    };
    char count_text[32];
    snprintf(count_text, sizeof(count_text), "%u device%s",
             (unsigned)s_home.visible_count,
             s_home.visible_count == 1 ? "" : "s");
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_DEVICE_COUNT, count_text);

    for (size_t slot = 0; slot < MATTER_PAGE_SIZE; ++slot) {
        const size_t visible_index = s_home.page * MATTER_PAGE_SIZE + slot;
        const bool visible = visible_index < s_home.visible_count;
        (void)esp_gsp_set_visible(ui, visible_binds[slot], visible);
        if (!visible) continue;
        const matter_vm_device_t *device =
            &s_home.devices[s_home.visible_indices[visible_index]];
        char status[MATTER_VM_TEXT_MAX + FABRIC_MANAGER_ROOM_MAX + 16U];
        snprintf(status, sizeof(status), "%s%s%s%s", device->type,
                 device->room[0] ? " | " : "", device->room,
                 device->online ? " | Online" : " | Offline");
        (void)esp_gsp_set_text(ui, name_binds[slot], device->name);
        (void)esp_gsp_set_text(ui, status_binds[slot], status);
    }
    const size_t page_count = s_home.visible_count == 0
        ? 1
        : (s_home.visible_count + MATTER_PAGE_SIZE - 1U) / MATTER_PAGE_SIZE;
    char page_text[32];
    snprintf(page_text, sizeof(page_text), "%u / %u",
             (unsigned)(s_home.page + 1U), (unsigned)page_count);
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_PAGE, page_text);
}

void matter_home_init(fabric_manager_handle_t manager)
{
    memset(&s_home, 0, sizeof(s_home));
    s_home.manager = manager;
    s_home.filter_all = true;
}

void matter_home_reset(void)
{
    fabric_manager_handle_t manager = s_home.manager;
    memset(&s_home, 0, sizeof(s_home));
    s_home.manager = manager;
    s_home.filter_all = true;
}

esp_err_t matter_home_refresh(esp_gsp_handle_t ui)
{
    if (!s_home.manager || !ui) return ESP_ERR_INVALID_STATE;
    char *json = NULL;
    esp_err_t error = fabric_manager_list_devices_json(s_home.manager, &json);
    if (error != ESP_OK) return error;
    error = matter_vm_parse_devices(json, s_home.devices,
                                    CONFIG_FABRIC_MANAGER_MAX_DEVICES,
                                    &s_home.device_count);
    fabric_manager_free_json(json);
    if (error != ESP_OK) return error;
    build_rooms();
    rebuild_visible();
    render_rooms(ui);
    render_devices(ui);
    return ESP_OK;
}

void matter_home_select_all(esp_gsp_handle_t ui)
{
    s_home.filter_all = true;
    s_home.filter_unassigned = false;
    s_home.filter_room[0] = '\0';
    s_home.page = 0;
    rebuild_visible();
    render_rooms(ui);
    render_devices(ui);
}

void matter_home_select_room(esp_gsp_handle_t ui, size_t slot)
{
    const size_t index = s_home.room_window + slot;
    if (index >= s_home.room_count) return;
    const matter_room_option_t *option = &s_home.room_options[index];
    s_home.filter_all = false;
    s_home.filter_unassigned = option->unassigned;
    snprintf(s_home.filter_room, sizeof(s_home.filter_room), "%s",
             option->room);
    s_home.page = 0;
    rebuild_visible();
    render_rooms(ui);
    render_devices(ui);
}

void matter_home_previous_rooms(esp_gsp_handle_t ui)
{
    if (s_home.room_window > 0) --s_home.room_window;
    render_rooms(ui);
}

void matter_home_next_rooms(esp_gsp_handle_t ui)
{
    if (s_home.room_window + MATTER_ROOM_SLOT_COUNT < s_home.room_count) {
        ++s_home.room_window;
    }
    render_rooms(ui);
}

void matter_home_previous_page(esp_gsp_handle_t ui)
{
    if (s_home.page > 0) --s_home.page;
    render_devices(ui);
}

void matter_home_next_page(esp_gsp_handle_t ui)
{
    if ((s_home.page + 1U) * MATTER_PAGE_SIZE < s_home.visible_count) {
        ++s_home.page;
    }
    render_devices(ui);
}

esp_err_t matter_home_device_at_slot(size_t slot,
                                     fabric_manager_device_id_t *out_device_id)
{
    if (!out_device_id || slot >= MATTER_PAGE_SIZE) return ESP_ERR_INVALID_ARG;
    const size_t visible_index = s_home.page * MATTER_PAGE_SIZE + slot;
    if (visible_index >= s_home.visible_count) return ESP_ERR_NOT_FOUND;
    *out_device_id =
        s_home.devices[s_home.visible_indices[visible_index]].id;
    return ESP_OK;
}
