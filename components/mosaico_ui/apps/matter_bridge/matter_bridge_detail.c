/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "matter_bridge_detail.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "matter_bridge_actions.h"
#include "matter_bridge_binds.h"
#include "matter_bridge_objects.h"
#include "matter_bridge_templates.h"

typedef enum {
    POWER, LEVEL, HUE, SATURATION, TEMPERATURE, MIN_MIRED, MAX_MIRED,
    CONTACT, OCCUPANCY, ILLUMINANCE, PROPERTY_COUNT
} property_t;

static const bridge_dev_mngr_attribute_path_t s_paths[PROPERTY_COUNT] = {
    [POWER] = {0x0006, 0}, [LEVEL] = {0x0008, 0},
    [HUE] = {0x0300, 0}, [SATURATION] = {0x0300, 1},
    [TEMPERATURE] = {0x0300, 7}, [MIN_MIRED] = {0x0300, 0x400b},
    [MAX_MIRED] = {0x0300, 0x400c}, [CONTACT] = {0x0045, 0},
    [OCCUPANCY] = {0x0406, 0}, [ILLUMINANCE] = {0x0400, 0},
};
static const char *const s_labels[PROPERTY_COUNT] = {
    "Power", "Brightness", "Hue / saturation", "Saturation", "Color temperature",
    "Minimum color temperature", "Maximum color temperature", "State", "Occupancy", "Illuminance",
};

typedef struct {
    bool pending;
    bool known;
    bridge_dev_mngr_value_t value;
} property_value_t;

typedef struct {
    uint32_t generation;
    property_t property;
    bool write;
} request_context_t;

/* Callbacks run on the Matter task. Only this mailbox crosses threads; all
 * scene updates and list binding stay on the Mosaic runtime owner. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_generation;
static property_value_t s_mailbox[PROPERTY_COUNT];
static bool s_write_pending;
static bool s_write_completed;
static esp_err_t s_write_error;

static bridge_dev_mngr_handle_t s_manager;
static bridge_dev_mngr_device_info_t s_device;
static property_value_t s_values[PROPERTY_COUNT];
static property_t s_rows[4];
static size_t s_row_count;
static size_t s_selected = SIZE_MAX;
static esp_gsp_list_t s_list = ESP_GSP_LIST_NONE;
static size_t s_list_total;
static char s_status[96];
static bool s_pointer_pressed;
static uint32_t s_queued_controls;
static uint32_t s_queued_values[TEMPERATURE + 1];

static void write_property(property_t property, const bridge_dev_mngr_value_t *value);

bool matter_bridge_device_is_camera(const bridge_dev_mngr_device_info_t *device)
{
    return device->definition.device_type == 0x0142 ||
           !strcmp(device->definition.profile_id, "mosaico_camera");
}

bool matter_bridge_device_has_controls(const bridge_dev_mngr_device_info_t *device)
{
    const char *profile = device->definition.profile_id;
    return !strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_ON_OFF_LIGHT) ||
           !strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_OUTLET) ||
           !strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_DIMMABLE_LIGHT) ||
           !strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT);
}

const char *matter_bridge_device_type(const bridge_dev_mngr_device_info_t *device)
{
    const char *profile = device->definition.profile_id;
    if (matter_bridge_device_is_camera(device)) return "Camera";
    if (!strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_ON_OFF_LIGHT)) return "On/off light";
    if (!strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_OUTLET)) return "Outlet";
    if (!strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_DIMMABLE_LIGHT)) return "Dimmable light";
    if (!strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT)) return "Extended color light";
    if (!strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_CONTACT_SENSOR)) return "Contact sensor";
    if (!strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_OCCUPANCY_SENSOR)) return "Occupancy sensor";
    if (!strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_ILLUMINANCE_SENSOR)) return "Light sensor";
    return "Device";
}

static void completed(bridge_dev_mngr_handle_t manager,
                       const bridge_dev_mngr_request_result_t *result, void *user_ctx)
{
    (void)manager;
    request_context_t *context = user_ctx;
    portENTER_CRITICAL(&s_lock);
    if (context->generation == s_generation) {
        if (context->write) {
            s_write_pending = false;
            s_write_completed = true;
            s_write_error = result->error;
        } else {
            property_value_t *property = &s_mailbox[context->property];
            property->pending = false;
            property->known = result->error == ESP_OK && !result->value.truncated &&
                              result->value.type != BRIDGE_DEV_MNGR_VALUE_NULL;
            property->value = result->value;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    free(context);
}

static void reset_detail_state(void)
{
    portENTER_CRITICAL(&s_lock);
    ++s_generation;
    memset(s_mailbox, 0, sizeof(s_mailbox));
    s_write_pending = false;
    s_write_completed = false;
    portEXIT_CRITICAL(&s_lock);
    memset(&s_device, 0, sizeof(s_device));
    memset(s_values, 0, sizeof(s_values));
    s_selected = SIZE_MAX;
    s_status[0] = 0;
    s_row_count = 0;
    s_pointer_pressed = false;
    s_queued_controls = 0;
}

void matter_bridge_detail_init(bridge_dev_mngr_handle_t manager)
{
    reset_detail_state();
    s_manager = manager;
    s_list = ESP_GSP_LIST_NONE;
    s_list_total = 0;
}

bool matter_bridge_detail_close(esp_gsp_handle_t ui)
{
    // Runtime list rows can outlive an ordinary parent layer's visibility.
    // Clear them before releasing their backing state or showing Home.
    if (s_list != ESP_GSP_LIST_NONE && s_list_total) {
        if (esp_gsp_list_set_total(ui, s_list, 0) != ESP_GSP_OK) return false;
        s_list_total = 0;
    }
    if (s_device.definition.id[0]) reset_detail_state();
    return true;
}

bool matter_bridge_detail_open(esp_gsp_handle_t ui,
                                const bridge_dev_mngr_device_info_t *device)
{
    if (!matter_bridge_detail_close(ui)) return false;
    s_device = *device;
    const char *profile = device->definition.profile_id;
    if (matter_bridge_device_has_controls(device)) {
        s_rows[s_row_count++] = POWER;
        if (!strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_DIMMABLE_LIGHT) ||
            !strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT)) s_rows[s_row_count++] = LEVEL;
        if (!strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT)) {
            s_rows[s_row_count++] = TEMPERATURE;
            s_rows[s_row_count++] = HUE;
        }
    } else if (!strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_CONTACT_SENSOR)) {
        s_rows[s_row_count++] = CONTACT;
    } else if (!strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_OCCUPANCY_SENSOR)) {
        s_rows[s_row_count++] = OCCUPANCY;
    } else if (!strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_ILLUMINANCE_SENSOR)) {
        s_rows[s_row_count++] = ILLUMINANCE;
    }
    return true;
}

static void read_property(property_t property)
{
    request_context_t *context = malloc(sizeof(*context));
    if (!context) return;
    portENTER_CRITICAL(&s_lock);
    if (s_mailbox[property].pending || s_write_pending) {
        portEXIT_CRITICAL(&s_lock);
        free(context);
        return;
    }
    *context = (request_context_t){.generation = s_generation, .property = property};
    s_mailbox[property].pending = true;
    portEXIT_CRITICAL(&s_lock);
    bridge_dev_mngr_request_id_t request;
    esp_err_t error = bridge_dev_mngr_read_attribute(s_manager, s_device.definition.id,
                         &s_paths[property], &request, completed, context);
    if (error != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_mailbox[property].pending = false;
        s_mailbox[property].known = false;
        portEXIT_CRITICAL(&s_lock);
        free(context);
    }
}

static bool number(property_t property, uint32_t *value)
{
    const property_value_t *item = &s_values[property];
    if (!item->known) return false;
    if (item->value.type == BRIDGE_DEV_MNGR_VALUE_UNSIGNED &&
        item->value.data.unsigned_value <= UINT32_MAX) {
        *value = (uint32_t)item->value.data.unsigned_value;
        return true;
    }
    return false;
}

static bool boolean(property_t property, bool *value)
{
    if (!s_values[property].known) return false;
    if (s_values[property].value.type == BRIDGE_DEV_MNGR_VALUE_BOOL) {
        *value = s_values[property].value.data.boolean;
        return true;
    }
    uint32_t bits;
    if (!number(property, &bits)) return false;
    *value = (bits & 1) != 0;
    return true;
}

static bool actionable(property_t property)
{
    return s_device.reachable && property <= TEMPERATURE;
}

static void format_value(property_t property, char *text, size_t capacity)
{
    uint32_t value;
    bool on;
    snprintf(text, capacity, "Unknown");
    if (property == POWER || property == CONTACT || property == OCCUPANCY) {
        if (boolean(property, &on)) snprintf(text, capacity, "%s",
            property == OCCUPANCY ? (on ? "Occupied" : "Unoccupied") : (on ? "On" : "Off"));
    } else if (number(property, &value)) {
        if (property == LEVEL || property == SATURATION) {
            if (value <= 254) snprintf(text, capacity, "%ld%%", lround(value * 100.0 / 254));
        } else if (property == HUE) {
            if (value <= 254) snprintf(text, capacity, "%ld°", lround(value * 360.0 / 254) % 360);
        } else if (property == TEMPERATURE) {
            if (value && value < 65535) snprintf(text, capacity, "%ld K", lround(1000000.0 / value));
        } else if (property == ILLUMINANCE && value < 65535) {
            snprintf(text, capacity, "%.1f lx", value ? pow(10.0, (value - 1.0) / 10000) : 0.0);
        }
    }
}

static gsp_err_t bind_row(esp_gsp_handle_t ui, esp_gsp_row_t row,
                           uint32_t position, void *user_ctx)
{
    (void)user_ctx;
    if (position >= s_row_count) return GSP_ERR_INVALID_ARG;
    property_t property = s_rows[position];
    char value[96];
    format_value(property, value, sizeof(value));
    esp_gsp_err_t error = gsp_matter_bridge_matter_bridge_detail_row_row_set_name_text(
        ui, row, s_labels[property]);
    if (error == ESP_GSP_OK) error = gsp_matter_bridge_matter_bridge_detail_row_row_set_value_text(ui, row, value);
    if (error == ESP_GSP_OK) error = gsp_matter_bridge_matter_bridge_detail_row_row_set_value_fg_color(
        ui, row, actionable(property) ? UINT16_C(0xFA40) : UINT16_C(0xB5B7));
    if (error == ESP_GSP_OK) error = gsp_matter_bridge_matter_bridge_detail_row_row_set_arrow_text(ui, row, "›");
    return error == ESP_GSP_OK ? GSP_OK : GSP_ERR_INVALID_STATE;
}

static void temperature_range(int32_t *minimum, int32_t *maximum)
{
    uint32_t low, high;
    *minimum = 2000;
    *maximum = 6500;
    if (number(MAX_MIRED, &high) && high) *minimum = (int32_t)lround(1000000.0 / high);
    if (number(MIN_MIRED, &low) && low) *maximum = (int32_t)lround(1000000.0 / low);
}

static void render_editor(esp_gsp_handle_t ui, bool pending)
{
    bool editing = s_selected < s_row_count;
    property_t property = editing ? s_rows[s_selected] : PROPERTY_COUNT;
    esp_gsp_set_visible(ui, GSP_BIND_MATTER_BRIDGE_ENTRIES_VISIBLE, !editing);
    esp_gsp_set_visible(ui, GSP_BIND_MATTER_BRIDGE_EDITOR_PAGE_VISIBLE, editing);
    esp_gsp_set_visible(ui, GSP_BIND_MATTER_BRIDGE_POWER_VISIBLE, editing && property == POWER);
    esp_gsp_set_visible(ui, GSP_BIND_MATTER_BRIDGE_BRIGHTNESS_VISIBLE, editing && property == LEVEL);
    esp_gsp_set_visible(ui, GSP_BIND_MATTER_BRIDGE_COLOR_TEMPERATURE_VISIBLE, editing && property == TEMPERATURE);
    esp_gsp_set_visible(ui, GSP_BIND_MATTER_BRIDGE_COLOR_VISIBLE, editing && property == HUE);
    esp_gsp_set_visible(ui, GSP_BIND_MATTER_BRIDGE_READONLY_VISIBLE, editing && property >= CONTACT);
    if (!editing) return;
    esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_EDITOR_TITLE, s_labels[property]);
    esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_ACTION_STATUS, pending ? "Sending..." : s_status);
    char text[96];
    format_value(property, text, sizeof(text));
    bool enabled = actionable(property);
    uint32_t raw;
    bool on;
    if (property == POWER) {
        bool known = boolean(POWER, &on);
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_POWER_STATE, text);
        if (known) gsp_matter_bridge_matter_bridge_power_toggle_set_checked(ui, on);
        esp_gsp_component_set_enabled(ui, GSP_OBJ_KEY_MATTER_BRIDGE_POWER_TAP, enabled && known && !pending);
    } else if (property == LEVEL) {
        bool known = number(LEVEL, &raw) && raw <= 254;
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_BRIGHTNESS_STATE, text);
        if (known && !s_pointer_pressed && !pending) gsp_matter_bridge_matter_bridge_brightness_slider_set_value(ui, (int32_t)lround(raw * 100.0 / 254));
        esp_gsp_component_set_enabled(ui, GSP_OBJ_KEY_MATTER_BRIDGE_BRIGHTNESS_SLIDER, enabled && known);
    } else if (property == TEMPERATURE) {
        int32_t minimum, maximum;
        temperature_range(&minimum, &maximum);
        bool known = number(TEMPERATURE, &raw) && raw && raw < 65535 && maximum > minimum;
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_COLOR_TEMPERATURE_STATE, text);
        if (known && !s_pointer_pressed && !pending) {
            int32_t percent = (int32_t)lround((1000000.0 / raw - minimum) * 100 / (maximum - minimum));
            gsp_matter_bridge_matter_bridge_color_temperature_slider_set_value(ui, percent < 0 ? 0 : percent > 100 ? 100 : percent);
        }
        snprintf(text, sizeof(text), "%d K", (int)minimum);
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_TEMPERATURE_WARM, text);
        snprintf(text, sizeof(text), "%d K", (int)maximum);
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_TEMPERATURE_COOL, text);
        esp_gsp_component_set_enabled(ui, GSP_OBJ_KEY_MATTER_BRIDGE_COLOR_TEMPERATURE_SLIDER, enabled && known);
    } else if (property == HUE) {
        bool hue_known = number(HUE, &raw) && raw <= 254;
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_HUE_TEXT, text);
        if (hue_known && !s_pointer_pressed && !pending) gsp_matter_bridge_matter_bridge_hue_slider_set_value(ui, (int32_t)lround(raw * 360.0 / 254) % 360);
        bool saturation_known = number(SATURATION, &raw) && raw <= 254;
        format_value(SATURATION, text, sizeof(text));
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_SATURATION_TEXT, text);
        if (saturation_known && !s_pointer_pressed && !pending) gsp_matter_bridge_matter_bridge_saturation_slider_set_value(ui, (int32_t)lround(raw * 100.0 / 254));
        esp_gsp_component_set_enabled(ui, GSP_OBJ_KEY_MATTER_BRIDGE_HUE_SLIDER, enabled && hue_known);
        esp_gsp_component_set_enabled(ui, GSP_OBJ_KEY_MATTER_BRIDGE_SATURATION_SLIDER, enabled && saturation_known);
    } else {
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_READONLY_VALUE, text);
    }
}

bool matter_bridge_detail_tick(esp_gsp_handle_t ui)
{
    if (!s_manager || !s_device.definition.id[0]) return false;
    bridge_dev_mngr_device_info_t current;
    if (bridge_dev_mngr_get_device(s_manager, s_device.definition.id, &current) != ESP_OK ||
        !current.active || current.endpoint_id != s_device.endpoint_id) return false;
    s_device = current;
    portENTER_CRITICAL(&s_lock);
    memcpy(s_values, s_mailbox, sizeof(s_values));
    bool pending = s_write_pending;
    bool write_completed = s_write_completed;
    esp_err_t error = s_write_error;
    s_write_completed = false;
    portEXIT_CRITICAL(&s_lock);
    if (write_completed) snprintf(s_status, sizeof(s_status), "%s", error == ESP_OK ? "" : esp_err_to_name(error));
    if (!pending && s_queued_controls) {
        for (property_t property = LEVEL; property <= TEMPERATURE; ++property) {
            if (!(s_queued_controls & (1U << property))) continue;
            bridge_dev_mngr_value_t value = {.type = BRIDGE_DEV_MNGR_VALUE_UNSIGNED};
            value.data.unsigned_value = s_queued_values[property];
            write_property(property, &value);
            portENTER_CRITICAL(&s_lock);
            pending = s_write_pending;
            portEXIT_CRITICAL(&s_lock);
            break;
        }
    }
    esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_DETAIL_NAME, s_device.definition.name);
    char status[96];
    snprintf(status, sizeof(status), "%s | %s", matter_bridge_device_type(&s_device),
             s_device.reachable ? "Online" : "Offline");
    esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_DETAIL_STATUS, status);
    esp_gsp_set_visible(ui, GSP_BIND_MATTER_BRIDGE_EMPTY_VISIBLE, s_row_count == 0);
    if (s_list == ESP_GSP_LIST_NONE) s_list = esp_gsp_list_bind_component(
        ui, GSP_OBJ_KEY_MATTER_BRIDGE_DETAIL_ROWS, bind_row, NULL);
    if (s_list != ESP_GSP_LIST_NONE) {
        size_t total = s_selected == SIZE_MAX ? s_row_count : 0;
        if (s_list_total != total) {
            if (esp_gsp_list_set_total(ui, s_list, total) != ESP_GSP_OK) return true;
            s_list_total = total;
            if (total) esp_gsp_list_scroll_to(ui, s_list, 0);
        }
        if (total) esp_gsp_list_refresh(ui, s_list);
    }
    render_editor(ui, pending);
    for (size_t index = 0; index < s_row_count; ++index) read_property(s_rows[index]);
    if (!strcmp(s_device.definition.profile_id, BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT)) {
        read_property(SATURATION);
        read_property(MIN_MIRED);
        read_property(MAX_MIRED);
    }
    return true;
}

void matter_bridge_detail_open_entry(size_t index)
{
    if (index < s_row_count) {
        s_selected = index;
        s_status[0] = 0;
    }
}

bool matter_bridge_detail_back(void)
{
    if (s_selected == SIZE_MAX) return false;
    s_selected = SIZE_MAX;
    s_queued_controls = 0;
    return true;
}

void matter_bridge_detail_control(uint16_t action, int32_t argument)
{
    if (s_selected >= s_row_count || !s_device.reachable) return;
    property_t selected = s_rows[s_selected];
    property_t property;
    bridge_dev_mngr_value_t value = {.type = BRIDGE_DEV_MNGR_VALUE_UNSIGNED};
    if (action == GSP_ACT_ID_MATTER_BRIDGE_POWER_TOGGLE && selected == POWER) {
        bool on;
        if (!boolean(POWER, &on)) return;
        property = POWER;
        value.type = BRIDGE_DEV_MNGR_VALUE_BOOL;
        value.data.boolean = !on;
    } else if (action == GSP_ACT_ID_MATTER_BRIDGE_LEVEL && selected == LEVEL && argument >= 1 && argument <= 100) {
        property = LEVEL;
        value.data.unsigned_value = (uint32_t)lround(argument * 254.0 / 100);
    } else if (action == GSP_ACT_ID_MATTER_BRIDGE_HUE && selected == HUE && argument >= 0 && argument <= 359) {
        property = HUE;
        value.data.unsigned_value = (uint32_t)lround(argument * 254.0 / 360);
    } else if (action == GSP_ACT_ID_MATTER_BRIDGE_SATURATION && selected == HUE && argument >= 0 && argument <= 100) {
        property = SATURATION;
        value.data.unsigned_value = (uint32_t)lround(argument * 254.0 / 100);
    } else if (action == GSP_ACT_ID_MATTER_BRIDGE_COLOR_TEMP && selected == TEMPERATURE && argument >= 0 && argument <= 100) {
        property = TEMPERATURE;
        int32_t minimum, maximum;
        temperature_range(&minimum, &maximum);
        value.data.unsigned_value = (uint32_t)lround(1000000.0 / (minimum + argument * (maximum - minimum) / 100.0));
    } else return;
    write_property(property, &value);
}

void matter_bridge_detail_pointer(bool pressed)
{
    s_pointer_pressed = pressed;
}

static void write_property(property_t property, const bridge_dev_mngr_value_t *value)
{
    request_context_t *context = malloc(sizeof(*context));
    if (!context) {
        snprintf(s_status, sizeof(s_status), "Out of memory");
        return;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_write_pending) {
        portEXIT_CRITICAL(&s_lock);
        free(context);
        if (property != POWER) {
            s_queued_values[property] = (uint32_t)value->data.unsigned_value;
            s_queued_controls |= 1U << property;
        }
        return;
    }
    *context = (request_context_t){.generation = s_generation, .property = property, .write = true};
    s_write_pending = true;
    s_write_completed = false;
    portEXIT_CRITICAL(&s_lock);
    // A fresh drag sample supersedes any older queued value for this control.
    s_queued_controls &= ~(1U << property);
    bridge_dev_mngr_request_id_t request;
    esp_err_t error = bridge_dev_mngr_write_attribute(s_manager, s_device.definition.id,
                         &s_paths[property], value, &request, completed, context);
    if (error != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_write_pending = false;
        portEXIT_CRITICAL(&s_lock);
        free(context);
        snprintf(s_status, sizeof(s_status), "%s", esp_err_to_name(error));
    }
}
