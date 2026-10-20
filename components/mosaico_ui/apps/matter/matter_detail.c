/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "matter_detail.h"

#include "matter_view_model.h"

#include "matter_binds.h"
#include "matter_objects.h"
#include "matter_templates.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#endif

/* The row API uses RGB565; toggle track colors use RGB888. */
#define MATTER_TEXT_MAX 256U
#define MATTER_ACCENT_RGB888 UINT32_C(0xFF4C01)
#define MATTER_ROW_MUTED_COLOR UINT32_C(0xB5B7)  /* #B5B5BF */
#define MATTER_TOGGLE_OFF_COLOR UINT32_C(0x3B3C3D)
/* Default Kelvin bounds match the Manager color-control action when not reported. */
#define TEMPERATURE_MIN_K 2000
#define TEMPERATURE_MAX_K 6500

/* The selected endpoint's supported actions determine which controls appear. */
typedef enum {
    MATTER_DETAIL_CONTROL_NONE = 0,
    MATTER_DETAIL_CONTROL_POWER,
    MATTER_DETAIL_CONTROL_BRIGHTNESS,
    MATTER_DETAIL_CONTROL_COLOR_TEMPERATURE,
    MATTER_DETAIL_CONTROL_HUE,
    MATTER_DETAIL_CONTROL_SATURATION,
    MATTER_DETAIL_CONTROL_MODE,
    MATTER_DETAIL_CONTROL_TARGET,
    MATTER_DETAIL_CONTROL_FAN,
    MATTER_DETAIL_CONTROL_RVC_MODES,
    MATTER_DETAIL_CONTROL_RVC_COMMANDS,
} matter_detail_control_t;

typedef struct {
    bool available;
    matter_vm_action_t value;
} matter_action_binding_t;

typedef struct {
    matter_action_binding_t action;
    bool known;
    bool on;
} matter_on_off_detail_t;

typedef struct {
    matter_action_binding_t action;
    bool known;
    int32_t percent;
} matter_level_detail_t;

typedef struct {
    matter_action_binding_t color_action;
    matter_action_binding_t temperature_action;
    bool hue_known;
    bool saturation_known;
    bool temperature_known;
    bool inline_temperature;
    bool inline_color;
    int32_t hue_degrees;
    int32_t saturation_percent;
    int32_t temperature_kelvin;
    int32_t min_kelvin;
    int32_t max_kelvin;
} matter_color_detail_t;

/* Internal control functions are grouped by cluster below. */
static bool focused_control(matter_detail_control_t control);
static void matter_on_off_update(matter_on_off_detail_t *state,
                                  const matter_vm_detail_t *detail);
static void matter_on_off_render(esp_gsp_handle_t ui,
                                  const matter_on_off_detail_t *state,
                                  bool online);
static esp_err_t matter_on_off_toggle(esp_gsp_handle_t ui,
                                       const matter_on_off_detail_t *state,
                                       bool online);
static void matter_level_update(matter_level_detail_t *state,
                                 const matter_vm_detail_t *detail);
static void matter_level_render(esp_gsp_handle_t ui,
                                 const matter_level_detail_t *state,
                                 bool online);
static void matter_level_set(esp_gsp_handle_t ui,
                              const matter_level_detail_t *state,
                              bool online, int32_t percent);
static void matter_color_update(matter_color_detail_t *state,
                                 const matter_vm_detail_t *detail);
static matter_detail_control_t matter_color_property_control(
    const matter_vm_property_t *property);
static bool matter_color_has_pair(const matter_vm_detail_t *detail,
                                   const matter_color_detail_t *state);
static const char *matter_color_row_label(const matter_vm_property_t *property);
static void matter_color_row_value(const matter_vm_property_t *property,
                                    char *text, size_t capacity);
static void matter_color_render_temperature(esp_gsp_handle_t ui,
                                             const matter_color_detail_t *state,
                                             bool online);
static void matter_color_render_controls(esp_gsp_handle_t ui,
                                          const matter_color_detail_t *state,
                                          bool online);
static void matter_color_set(esp_gsp_handle_t ui, matter_color_detail_t *state,
                              bool online, int32_t hue, int32_t saturation);
static void matter_color_set_hue(esp_gsp_handle_t ui,
                                  matter_color_detail_t *state,
                                  bool online, int32_t hue);
static void matter_color_set_saturation(esp_gsp_handle_t ui,
                                         matter_color_detail_t *state,
                                         bool online, int32_t saturation);
static void matter_color_set_temperature(esp_gsp_handle_t ui,
                                          const matter_color_detail_t *state,
                                          bool online, int32_t percent);
static void matter_color_render_hue_editor(esp_gsp_handle_t ui,
                                             int32_t degrees);
static void matter_color_render_saturation_editor(esp_gsp_handle_t ui,
                                                    int32_t percent);

typedef struct {
    uint16_t endpoint;
    char cluster[FABRIC_MANAGER_CLUSTER_KEY_MAX];
    char key[FABRIC_MANAGER_MEMBER_KEY_MAX];
    matter_detail_control_t control;
    char name[MATTER_VM_TEXT_MAX];
    char value[MATTER_VM_VALUE_MAX];
} matter_state_row_t;

typedef enum {
    RVC_START_NONE,
    RVC_START_CLEAN,
    RVC_START_RUN,
    RVC_START_WAIT_STATE,
} rvc_start_phase_t;

typedef struct {
    uint32_t generation;
    rvc_start_phase_t phase;
} rvc_start_context_t;

typedef struct {
    fabric_manager_handle_t manager;
    fabric_manager_device_id_t device_id;
    matter_vm_detail_t detail;
    matter_on_off_detail_t on_off;
    matter_level_detail_t level;
    matter_color_detail_t color;
    matter_state_row_t *state_rows;
    size_t state_row_count;
    size_t selected_entry;
    uint16_t selected_endpoint;
    char selected_cluster[FABRIC_MANAGER_CLUSTER_KEY_MAX];
    char selected_key[FABRIC_MANAGER_MEMBER_KEY_MAX];
    esp_gsp_list_t detail_list;
    size_t list_total;
    bool detail_scroll_reset_pending;
    char action_status[96];
    bool rvc_draft_ready;
    uint16_t rvc_draft_endpoint;
    size_t rvc_run_choice;
    size_t rvc_clean_choice;
    rvc_start_phase_t rvc_start_phase;
    uint32_t rvc_start_generation;
    bool rvc_start_completion;
    esp_err_t rvc_start_error;
#if defined(ESP_PLATFORM)
    portMUX_TYPE event_lock;
#endif
    uint32_t property_generation;
    uint32_t action_property_generation;
    bool action_waiting_report;
    bool completion_pending;
    esp_err_t completion_error;
} matter_detail_state_t;

static matter_detail_state_t s_detail = {
    .selected_entry = SIZE_MAX,
    .detail_list = ESP_GSP_LIST_NONE,
#if defined(ESP_PLATFORM)
    .event_lock = portMUX_INITIALIZER_UNLOCKED,
#endif
};

const matter_vm_property_t *matter_detail_bound_property(
    const matter_vm_detail_t *detail, const matter_action_binding_t *binding,
    const char *key)
{
    if (!binding->available) return NULL;
    return matter_vm_find_property(detail, binding->value.endpoint,
                                   binding->value.cluster, key);
}

bool matter_detail_action_matches(const matter_action_binding_t *binding,
                                   const matter_vm_property_t *property,
                                   const char *key)
{
    if (!binding->available || strcmp(property->key, key) ||
        binding->value.endpoint != property->endpoint ||
        strcmp(binding->value.cluster, property->cluster)) return false;
    for (size_t index = 0; index < binding->value.argument_count; ++index) {
        if (!strcmp(binding->value.arguments[index].key, key)) return true;
    }
    return false;
}

void matter_detail_bind_action(matter_action_binding_t *binding,
                               const matter_vm_detail_t *detail,
                               const char *cluster, const char *key)
{
    memset(binding, 0, sizeof(*binding));
    const matter_vm_action_t *action = matter_vm_find_action(detail, cluster, key);
    if (action) {
        binding->available = true;
        binding->value = *action;
    }
}

static void update_control_state(void)
{
    matter_on_off_update(&s_detail.on_off, &s_detail.detail);
    matter_level_update(&s_detail.level, &s_detail.detail);
    matter_color_update(&s_detail.color, &s_detail.detail);
}

static void free_state_rows(void)
{
    free(s_detail.state_rows);
    s_detail.state_rows = NULL;
    s_detail.state_row_count = 0;
    s_detail.color.inline_temperature = false;
    s_detail.color.inline_color = false;
}

static void disambiguate_repeated_property(const matter_vm_property_t *property,
                                           matter_state_row_t *row)
{
    bool repeated = false;
    bool named = property->endpoint_name[0] != '\0';
    for (size_t index = 0; index < s_detail.detail.property_count; ++index) {
        const matter_vm_property_t *other = &s_detail.detail.properties[index];
        if (other->endpoint == property->endpoint ||
            strcmp(other->cluster, property->cluster) ||
            strcmp(other->key, property->key)) continue;
        repeated = true;
        if (!strcmp(property->endpoint_name, other->endpoint_name)) named = false;
    }
    if (!repeated) return;
    char label[sizeof(row->name)];
    snprintf(label, sizeof(label), "%s", row->name);
    if (named) {
        snprintf(row->name, sizeof(row->name), "%.24s: %.37s",
                 property->endpoint_name, label);
    } else {
        snprintf(row->name, sizeof(row->name), "Endpoint %u: %.45s",
                 (unsigned)property->endpoint, label);
    }
}

static bool property_is_covered(const matter_vm_property_t *property)
{
    if (s_detail.color.inline_color &&
        matter_detail_action_matches(&s_detail.color.color_action,
                                     property, "saturation_percent")) return true;
    if (!strcmp(property->cluster, "fan_control") &&
        strcmp(property->key, "mode") &&
        matter_vm_find_property(&s_detail.detail, property->endpoint,
                                "fan_control", "mode")) return true;
    return !strcmp(property->cluster, "rvc_clean_mode") &&
        matter_vm_find_property(&s_detail.detail, property->endpoint,
                                "rvc_run_mode", "mode");
}

static matter_detail_control_t state_row_control(
    const matter_vm_property_t *property,
    const matter_vm_property_t *temperature)
{
    if (matter_detail_action_matches(&s_detail.on_off.action, property, "on"))
        return MATTER_DETAIL_CONTROL_POWER;
    if (matter_detail_action_matches(&s_detail.level.action, property,
                                     "level_percent"))
        return MATTER_DETAIL_CONTROL_BRIGHTNESS;
    if (s_detail.color.inline_temperature && property == temperature)
        return MATTER_DETAIL_CONTROL_COLOR_TEMPERATURE;
    if (s_detail.color.inline_color &&
        matter_detail_action_matches(&s_detail.color.color_action,
                                     property, "hue_degrees"))
        return MATTER_DETAIL_CONTROL_HUE;
    if (!strcmp(property->cluster, "thermostat")) {
        if (!strcmp(property->key, "system_mode")) return MATTER_DETAIL_CONTROL_MODE;
        if (!strcmp(property->key, "heating_setpoint_c") ||
            !strcmp(property->key, "cooling_setpoint_c")) return MATTER_DETAIL_CONTROL_TARGET;
    }
    if (!strcmp(property->cluster, "fan_control")) return MATTER_DETAIL_CONTROL_FAN;
    if ((!strcmp(property->cluster, "rvc_run_mode") ||
         !strcmp(property->cluster, "rvc_clean_mode")) &&
        !strcmp(property->key, "mode")) return MATTER_DETAIL_CONTROL_RVC_MODES;
    return MATTER_DETAIL_CONTROL_NONE;
}

static void fill_property_row(const matter_vm_property_t *property,
                              const matter_vm_property_t *temperature,
                              matter_state_row_t *row)
{
    row->endpoint = property->endpoint;
    snprintf(row->cluster, sizeof(row->cluster), "%s", property->cluster);
    snprintf(row->key, sizeof(row->key), "%s", property->key);
    row->control = state_row_control(property, temperature);
    if (row->control == MATTER_DETAIL_CONTROL_HUE) {
        snprintf(row->name, sizeof(row->name), "Hue / saturation");
        if (s_detail.color.hue_known && s_detail.color.saturation_known) {
            snprintf(row->value, sizeof(row->value), "%ld° / %ld%%",
                     (long)s_detail.color.hue_degrees,
                     (long)s_detail.color.saturation_percent);
        } else {
            snprintf(row->value, sizeof(row->value), "Waiting for report");
        }
    } else if (!strcmp(property->cluster, "level_control") &&
               !strcmp(property->key, "level_percent")) {
        snprintf(row->name, sizeof(row->name), "Level");
        if (property->value_kind == MATTER_VM_VALUE_NUMBER &&
            property->number_value >= 0 && property->number_value <= 100) {
            snprintf(row->value, sizeof(row->value), "%ld%%",
                     (long)lround(property->number_value));
        } else {
            matter_color_row_value(property, row->value, sizeof(row->value));
        }
    } else {
        const char *name = matter_color_row_label(property);
        snprintf(row->name, sizeof(row->name), "%s",
                 name ? name : property->name);
        matter_color_row_value(property, row->value, sizeof(row->value));
    }
    if (!strcmp(property->cluster, "thermostat")) {
        if (!strcmp(property->key, "local_temperature_c"))
            snprintf(row->name, sizeof(row->name), "Local temperature");
        else if (!strcmp(property->key, "system_mode"))
            snprintf(row->name, sizeof(row->name), "Mode");
        else if (!strcmp(property->key, "heating_setpoint_c"))
            snprintf(row->name, sizeof(row->name), "Heat target");
        else if (!strcmp(property->key, "cooling_setpoint_c"))
            snprintf(row->name, sizeof(row->name), "Cool target");
    } else if (!strcmp(property->cluster, "fan_control")) {
        snprintf(row->name, sizeof(row->name), "Fan Control");
        const matter_vm_property_t *mode = matter_vm_find_property(
            &s_detail.detail, property->endpoint, "fan_control", "mode");
        snprintf(row->value, sizeof(row->value), "%s",
                 mode ? mode->display_value : property->display_value);
    } else if (row->control == MATTER_DETAIL_CONTROL_RVC_MODES) {
        snprintf(row->name, sizeof(row->name), "Modes");
        snprintf(row->value, sizeof(row->value), "Run / Clean");
    } else if (!strcmp(property->cluster, "rvc_operational_state") &&
               !strcmp(property->key, "state")) {
        snprintf(row->name, sizeof(row->name), "State");
    }
    if (!s_detail.detail.device.online)
        snprintf(row->value, sizeof(row->value), "Offline");
    disambiguate_repeated_property(property, row);
}

static void append_state_row(void)
{
    const matter_state_row_t *row = &s_detail.state_rows[s_detail.state_row_count];
    if (s_detail.selected_cluster[0] &&
        row->endpoint == s_detail.selected_endpoint &&
        !strcmp(row->cluster, s_detail.selected_cluster) &&
        !strcmp(row->key, s_detail.selected_key))
        s_detail.selected_entry = s_detail.state_row_count;
    ++s_detail.state_row_count;
}

static esp_err_t build_state_rows(void)
{
    free_state_rows();
    s_detail.selected_entry = SIZE_MAX;
    if (!s_detail.detail.property_count) return ESP_OK;
    s_detail.state_rows = calloc(s_detail.detail.property_count * 2,
                                 sizeof(*s_detail.state_rows));
    if (!s_detail.state_rows) return ESP_ERR_NO_MEM;
    s_detail.color.inline_color = matter_color_has_pair(&s_detail.detail,
                                                          &s_detail.color);
    const matter_vm_property_t *temperature = matter_detail_bound_property(
        &s_detail.detail, &s_detail.color.temperature_action,
        "color_temperature_kelvin");
    s_detail.color.inline_temperature = temperature &&
        matter_detail_action_matches(&s_detail.color.temperature_action,
                                     temperature, "color_temperature_kelvin");
    for (size_t index = 0; index < s_detail.detail.property_count; ++index) {
        const matter_vm_property_t *property = &s_detail.detail.properties[index];
        if (property_is_covered(property)) continue;
        matter_state_row_t *row = &s_detail.state_rows[s_detail.state_row_count];
        fill_property_row(property, temperature, row);
        append_state_row();
    }
    for (size_t index = 0; index < s_detail.detail.property_count; ++index) {
        const matter_vm_property_t *property = &s_detail.detail.properties[index];
        if (!strcmp(property->cluster, "rvc_operational_state") &&
            !strcmp(property->key, "state")) {
            matter_state_row_t *command = &s_detail.state_rows[s_detail.state_row_count];
            command->endpoint = property->endpoint;
            snprintf(command->cluster, sizeof(command->cluster), "%s", property->cluster);
            snprintf(command->key, sizeof(command->key), "control");
            snprintf(command->name, sizeof(command->name), "Control");
            snprintf(command->value, sizeof(command->value), "Actions");
            command->control = MATTER_DETAIL_CONTROL_RVC_COMMANDS;
            append_state_row();
        }
    }
    if (s_detail.selected_cluster[0] && s_detail.selected_entry == SIZE_MAX) {
        s_detail.selected_cluster[0] = '\0';
    }
    return ESP_OK;
}

static uint32_t rgb565_from_rgb888(uint32_t rgb)
{
    return ((rgb & UINT32_C(0xF80000)) >> 8) |
           ((rgb & UINT32_C(0x00FC00)) >> 5) |
           ((rgb & UINT32_C(0x0000F8)) >> 3);
}

static bool row_is_actionable(const matter_state_row_t *row)
{
    if (!s_detail.detail.device.online) return false;
    switch (row->control) {
    case MATTER_DETAIL_CONTROL_POWER:
        return s_detail.on_off.known;
    case MATTER_DETAIL_CONTROL_BRIGHTNESS:
        return s_detail.level.known;
    case MATTER_DETAIL_CONTROL_COLOR_TEMPERATURE:
        return s_detail.color.temperature_known;
    case MATTER_DETAIL_CONTROL_HUE:
        return s_detail.color.hue_known && s_detail.color.saturation_known;
    case MATTER_DETAIL_CONTROL_MODE:
    case MATTER_DETAIL_CONTROL_TARGET:
    case MATTER_DETAIL_CONTROL_FAN:
    case MATTER_DETAIL_CONTROL_RVC_MODES:
    case MATTER_DETAIL_CONTROL_RVC_COMMANDS:
        for (size_t i = 0; s_detail.detail.actions &&
                           i < s_detail.detail.action_count; ++i) {
            const matter_vm_action_t *action = &s_detail.detail.actions[i];
            if (action->endpoint == row->endpoint &&
                !strcmp(action->cluster, row->cluster)) return true;
        }
        return false;
    default:
        return false;
    }
}

static gsp_err_t bind_state_item(esp_gsp_handle_t ui, esp_gsp_row_t row,
                                 uint32_t position, void *user_ctx)
{
    (void)user_ctx;
    if (position >= s_detail.state_row_count) return GSP_ERR_INVALID_ARG;
    const matter_state_row_t *state_row = &s_detail.state_rows[position];
    const bool actionable = row_is_actionable(state_row);
    esp_gsp_err_t error = gsp_matter_matter_detail_row_row_set_name_text(
        ui, row, state_row->name);
    if (error == ESP_GSP_OK) {
        error = gsp_matter_matter_detail_row_row_set_value_text(
            ui, row, state_row->value);
    }
    if (error == ESP_GSP_OK) {
        error = gsp_matter_matter_detail_row_row_set_value_fg_color(
            ui, row, actionable
                         ? rgb565_from_rgb888(MATTER_ACCENT_RGB888)
                         : MATTER_ROW_MUTED_COLOR);
    }
    if (error == ESP_GSP_OK) {
        error = gsp_matter_matter_detail_row_row_set_arrow_text(
            ui, row, actionable ? "›" : "");
    }
#if !defined(ESP_PLATFORM)
    fprintf(stderr, "matter detail row %u: %s / %s rc=%d\n",
            (unsigned)position, state_row->name, state_row->value,
            (int)error);
#endif
    return error == ESP_GSP_OK ? GSP_OK : GSP_ERR_INVALID_STATE;
}

static const char *detail_type(void)
{
    if (!strcmp(s_detail.detail.device.type, "light")) {
        if (s_detail.on_off.action.available &&
            s_detail.level.action.available && !s_detail.color.color_action.available &&
            !s_detail.color.temperature_action.available) {
            return "Dimmable light";
        }
        if (s_detail.on_off.action.available &&
            s_detail.level.action.available &&
            s_detail.color.color_action.available &&
            s_detail.color.temperature_action.available) {
            return "Extended color light";
        }
        if (s_detail.on_off.action.available &&
            s_detail.level.action.available &&
            s_detail.color.temperature_action.available &&
            !s_detail.color.color_action.available) {
            return "Color temperature light";
        }
        if (s_detail.on_off.action.available &&
            !s_detail.color.color_action.available && !s_detail.color.temperature_action.available) {
            return "On/off light";
        }
    }
    return s_detail.detail.device.type;
}

static esp_err_t load_detail(esp_gsp_handle_t ui)
{
    if (!s_detail.manager || !s_detail.device_id) return ESP_ERR_INVALID_STATE;
    char *json = NULL;
    esp_err_t error = fabric_manager_get_device_json(s_detail.manager,
                                              s_detail.device_id, &json);
    if (error != ESP_OK) return error;
    matter_vm_detail_t next = {0};
    error = matter_vm_parse_detail(json, &next);
    fabric_manager_free_json(json);
    if (error != ESP_OK) return error;

    /* List callbacks read the current rows and detail on the render task.
       Detach them before replacing either backing allocation. */
    if (s_detail.detail_list != ESP_GSP_LIST_NONE && s_detail.list_total) {
        if (esp_gsp_list_set_total(ui, s_detail.detail_list, 0) != ESP_GSP_OK) {
            matter_vm_free_detail(&next);
            return ESP_FAIL;
        }
        s_detail.list_total = 0;
    }
    free_state_rows();
    matter_vm_free_detail(&s_detail.detail);
    s_detail.detail = next;
    update_control_state();
    return build_state_rows();
}

static void set_completion(esp_err_t error)
{
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_detail.event_lock);
#endif
    s_detail.completion_error = error;
    s_detail.completion_pending = true;
#if defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&s_detail.event_lock);
#endif
}

static void action_complete(fabric_core_handle_t core,
                            const fabric_core_request_result_t *result,
                            void *user_ctx)
{
    (void)core;
    (void)user_ctx;
    set_completion(result->error);
}

void matter_detail_init(fabric_manager_handle_t manager)
{
    s_detail.manager = manager;
    /* Bindings belong to the GSP instance, not to an opened device. */
    s_detail.detail_list = ESP_GSP_LIST_NONE;
    s_detail.list_total = 0;
}

void matter_detail_close(esp_gsp_handle_t ui)
{
    /* A bound List can retain its rows when an ordinary layer is hidden. */
    if (ui && s_detail.detail_list != ESP_GSP_LIST_NONE &&
        s_detail.list_total != 0) {
        esp_gsp_err_t error = esp_gsp_list_set_total(ui, s_detail.detail_list, 0);
        if (error != ESP_GSP_OK) {
#if defined(ESP_PLATFORM)
            ESP_LOGW("matter_detail", "clear list failed: 0x%x", (unsigned)error);
#endif
            return; /* The List still owns the old backing rows. */
        }
        s_detail.list_total = 0;
    }
    free_state_rows();
    matter_vm_free_detail(&s_detail.detail);
    s_detail.device_id = 0;
    s_detail.selected_entry = SIZE_MAX;
    s_detail.selected_cluster[0] = '\0';
    s_detail.action_status[0] = '\0';
    s_detail.rvc_draft_ready = false;
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_detail.event_lock);
#endif
    ++s_detail.rvc_start_generation;
    s_detail.rvc_start_phase = RVC_START_NONE;
    s_detail.rvc_start_completion = false;
#if defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&s_detail.event_lock);
#endif
}

esp_err_t matter_detail_open(esp_gsp_handle_t ui,
                             fabric_manager_device_id_t device_id)
{
    matter_detail_close(ui);
    s_detail.device_id = device_id;
    s_detail.detail_scroll_reset_pending = true;
    return matter_detail_refresh(ui);
}

esp_err_t matter_detail_open_entry(esp_gsp_handle_t ui, size_t index)
{
    if (!ui || index >= s_detail.state_row_count) return ESP_ERR_INVALID_ARG;
    const matter_state_row_t *row = &s_detail.state_rows[index];
    s_detail.selected_endpoint = row->endpoint;
    snprintf(s_detail.selected_cluster, sizeof(s_detail.selected_cluster),
             "%s", row->cluster);
    snprintf(s_detail.selected_key, sizeof(s_detail.selected_key),
             "%s", row->key);
    s_detail.selected_entry = index;
    return matter_detail_refresh(ui);
}

bool matter_detail_back(esp_gsp_handle_t ui)
{
    if (!ui || s_detail.selected_entry == SIZE_MAX) return false;
    s_detail.selected_entry = SIZE_MAX;
    s_detail.selected_cluster[0] = '\0';
    (void)matter_detail_refresh(ui);
    return true;
}

static const matter_vm_action_t *detail_action(uint16_t endpoint,
                                                const char *cluster,
                                                const char *key)
{
    for (size_t i = 0; i < s_detail.detail.action_count; ++i) {
        const matter_vm_action_t *action = &s_detail.detail.actions[i];
        if (action->endpoint == endpoint && !strcmp(action->cluster, cluster) &&
            !strcmp(action->key, key)) return action;
    }
    return NULL;
}

static const matter_vm_property_t *detail_property(uint16_t endpoint,
                                                    const char *cluster,
                                                    const char *key)
{
    return matter_vm_find_property(&s_detail.detail, endpoint, cluster, key);
}

static bool mode_choice_index(const matter_vm_action_t *action,
                              const matter_vm_property_t *property, size_t *index)
{
    if (!action || !property || action->argument_count != 1 ||
        strcmp(action->arguments[0].key, "mode")) return false;
    const matter_vm_argument_t *argument = &action->arguments[0];
    for (size_t i = 0; i < argument->enum_count; ++i) {
        if ((argument->kind == MATTER_VM_ARGUMENT_INTEGER &&
             property->value_kind == MATTER_VM_VALUE_NUMBER &&
             property->number_value == argument->enum_ids[i]) ||
            (argument->kind == MATTER_VM_ARGUMENT_STRING &&
             property->value_kind == MATTER_VM_VALUE_STRING &&
             !strcmp(property->string_value, argument->enum_values[i]))) {
            *index = i;
            return true;
        }
    }
    return false;
}

static bool rvc_phase_is_idle(uint16_t endpoint)
{
    const matter_vm_property_t *state = detail_property(endpoint,
                                                "rvc_operational_state", "state");
    if (!s_detail.detail.device.online || !state ||
        state->value_kind != MATTER_VM_VALUE_STRING) return false;
    return !strcmp(state->string_value, "stopped") ||
           !strcmp(state->string_value, "docked") ||
           !strcmp(state->string_value, "charging");
}

static bool rvc_modes_editable(uint16_t endpoint)
{
    return s_detail.rvc_start_phase == RVC_START_NONE &&
           rvc_phase_is_idle(endpoint);
}

static void rvc_prepare_modes(uint16_t endpoint)
{
    if (s_detail.rvc_draft_ready && s_detail.rvc_draft_endpoint == endpoint)
        return;
    s_detail.rvc_draft_ready = false;
    const matter_vm_action_t *run = detail_action(endpoint, "rvc_run_mode", "set_mode");
    const matter_vm_action_t *clean = detail_action(endpoint, "rvc_clean_mode", "set_mode");
    if (!run || !clean || run->argument_count != 1 || clean->argument_count != 1 ||
        run->arguments[0].kind != MATTER_VM_ARGUMENT_INTEGER ||
        clean->arguments[0].kind != MATTER_VM_ARGUMENT_INTEGER ||
        strcmp(run->arguments[0].key, "mode") ||
        strcmp(clean->arguments[0].key, "mode") ||
        !run->arguments[0].enum_count || !clean->arguments[0].enum_count) return;
    size_t choice = 0;
    const matter_vm_property_t *run_property = detail_property(endpoint,
                                                           "rvc_run_mode", "mode");
    if (!mode_choice_index(run, run_property, &choice) ||
        run->arguments[0].enum_ids[choice] == 0) {
        choice = 0;
        while (choice < run->arguments[0].enum_count &&
               run->arguments[0].enum_ids[choice] == 0) ++choice;
    }
    if (choice == run->arguments[0].enum_count) return;
    s_detail.rvc_run_choice = choice;
    const matter_vm_property_t *clean_property = detail_property(endpoint,
                                                             "rvc_clean_mode", "mode");
    s_detail.rvc_clean_choice = 0;
    (void)mode_choice_index(clean, clean_property, &s_detail.rvc_clean_choice);
    s_detail.rvc_draft_endpoint = endpoint;
    s_detail.rvc_draft_ready = true;
}

static void rvc_start_complete(fabric_core_handle_t core,
                               const fabric_core_request_result_t *result,
                               void *user_ctx)
{
    (void)core;
    rvc_start_context_t *context = user_ctx;
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_detail.event_lock);
#endif
    if (context->generation == s_detail.rvc_start_generation &&
        context->phase == s_detail.rvc_start_phase) {
        s_detail.rvc_start_error = result->error;
        s_detail.rvc_start_completion = true;
    }
#if defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&s_detail.event_lock);
#endif
    free(context);
}

static esp_err_t rvc_send_start_mode(esp_gsp_handle_t ui, uint16_t endpoint,
                                     rvc_start_phase_t phase)
{
    const char *cluster = phase == RVC_START_CLEAN ? "rvc_clean_mode" : "rvc_run_mode";
    const matter_vm_action_t *action = detail_action(endpoint, cluster, "set_mode");
    if (!action || !s_detail.rvc_draft_ready) return ESP_ERR_INVALID_STATE;
    size_t choice = phase == RVC_START_CLEAN ? s_detail.rvc_clean_choice
                                             : s_detail.rvc_run_choice;
    if (choice >= action->arguments[0].enum_count) return ESP_ERR_INVALID_STATE;
    rvc_start_context_t *context = malloc(sizeof(*context));
    if (!context) return ESP_ERR_NO_MEM;
    context->generation = s_detail.rvc_start_generation;
    context->phase = phase;
    char arguments[48];
    snprintf(arguments, sizeof(arguments), "{\"mode\":%ld}",
             (long)action->arguments[0].enum_ids[choice]);
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_detail.event_lock);
#endif
    s_detail.rvc_start_phase = phase;
    s_detail.rvc_start_completion = false;
#if defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&s_detail.event_lock);
#endif
    fabric_core_request_id_t request_id = 0;
    esp_err_t error = fabric_manager_invoke_action(s_detail.manager, s_detail.device_id,
        endpoint, cluster, "set_mode", arguments, &request_id,
        rvc_start_complete, context);
    if (error != ESP_OK) {
#if defined(ESP_PLATFORM)
        portENTER_CRITICAL(&s_detail.event_lock);
#endif
        s_detail.rvc_start_phase = RVC_START_NONE;
        s_detail.rvc_start_completion = false;
#if defined(ESP_PLATFORM)
        portEXIT_CRITICAL(&s_detail.event_lock);
#endif
        free(context);
    }
    snprintf(s_detail.action_status, sizeof(s_detail.action_status), "%s",
             error == ESP_OK ? "Request submitted" : "Request failed");
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_ACTION_STATUS,
                           s_detail.action_status);
    return error;
}

static const char *mode_cluster(const matter_state_row_t *selected, bool second)
{
    if (selected->control == MATTER_DETAIL_CONTROL_FAN) return "fan_control";
    if (selected->control == MATTER_DETAIL_CONTROL_MODE) return "thermostat";
    if (selected->control != MATTER_DETAIL_CONTROL_RVC_MODES) return NULL;
    if (second) return "rvc_clean_mode";
    return detail_property(selected->endpoint, "rvc_run_mode", "mode")
               ? "rvc_run_mode" : "rvc_clean_mode";
}

static void show_mode_button(esp_gsp_handle_t ui, uint16_t endpoint,
                             const char *cluster, const char *prefix,
                             uint16_t visible_bind, uint16_t text_bind,
                             gsp_component_key_t previous_key,
                             gsp_component_key_t next_key)
{
    const matter_vm_property_t *property = detail_property(
        endpoint, cluster, !strcmp(cluster, "thermostat") ? "system_mode" : "mode");
    const matter_vm_action_t *action = detail_action(endpoint, cluster, "set_mode");
    (void)esp_gsp_set_visible(ui, visible_bind, property != NULL);
    if (!property) return;
    char text[MATTER_VM_VALUE_MAX + 32];
    const char *value = property->display_value;
    if ((!strcmp(cluster, "rvc_run_mode") || !strcmp(cluster, "rvc_clean_mode")) &&
        s_detail.rvc_draft_ready && action &&
        (rvc_modes_editable(endpoint) ||
         s_detail.rvc_start_phase != RVC_START_NONE)) {
        size_t choice = !strcmp(cluster, "rvc_run_mode") ? s_detail.rvc_run_choice
                                                         : s_detail.rvc_clean_choice;
        if (choice < action->arguments[0].enum_count)
            value = action->arguments[0].enum_values[choice];
    }
    snprintf(text, sizeof(text), "%s: %.72s", prefix, value);
    (void)esp_gsp_set_text(ui, text_bind, text);
    size_t index = 0;
    bool is_rvc = !strcmp(cluster, "rvc_run_mode") ||
                  !strcmp(cluster, "rvc_clean_mode");
    bool enabled = s_detail.detail.device.online && action &&
        (is_rvc ? (rvc_modes_editable(endpoint) && s_detail.rvc_draft_ready)
                : mode_choice_index(action, property, &index)) &&
        action->arguments[0].enum_count > 1;
    (void)esp_gsp_component_set_enabled(ui, previous_key, enabled);
    (void)esp_gsp_component_set_enabled(ui, next_key, enabled);
}

static bool rvc_can_go_home(uint16_t endpoint)
{
    const matter_vm_property_t *state = detail_property(endpoint,
                                                "rvc_operational_state", "state");
    if (!state || state->value_kind != MATTER_VM_VALUE_STRING ||
        !state->string_value[0] || !strcmp(state->string_value, "unknown"))
        return false;
    const char *phase = state->string_value;
    return strcmp(phase, "docked") && strcmp(phase, "charging") &&
           strcmp(phase, "seeking_charger") &&
           detail_action(endpoint, "rvc_operational_state", "go_home");
}

static void render_rvc_commands(esp_gsp_handle_t ui, uint16_t endpoint,
                                bool visible)
{
    (void)esp_gsp_set_visible(ui, GSP_BIND_MATTER_PAUSE_BUTTON_VISIBLE, visible);
    (void)esp_gsp_set_visible(ui, GSP_BIND_MATTER_HOME_BUTTON_VISIBLE, visible);
    if (!visible) return;
    const matter_vm_property_t *state = detail_property(endpoint,
                                                "rvc_operational_state", "state");
    const char *phase = state && state->value_kind == MATTER_VM_VALUE_STRING
                            ? state->string_value : "";
    const char *pause_key = !strcmp(phase, "paused") ? "resume" : "pause";
    bool start_allowed = rvc_modes_editable(endpoint) && s_detail.rvc_draft_ready;
    bool pause_allowed = (!strcmp(phase, "running") || !strcmp(phase, "paused")) &&
        detail_action(endpoint, "rvc_operational_state", pause_key);
    bool home_allowed = rvc_can_go_home(endpoint) &&
        s_detail.rvc_start_phase == RVC_START_NONE;
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_PAUSE_BUTTON_TEXT,
                           !strcmp(phase, "paused") ? "Resume" :
                           !strcmp(phase, "running") ? "Pause" : "Start");
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_HOME_BUTTON_TEXT, "Go home");
    (void)esp_gsp_component_set_enabled(ui, GSP_OBJ_KEY_MATTER_PAUSE_BUTTON,
        s_detail.detail.device.online && (start_allowed || pause_allowed) &&
        s_detail.rvc_start_phase == RVC_START_NONE);
    (void)esp_gsp_component_set_enabled(ui, GSP_OBJ_KEY_MATTER_HOME_BUTTON,
                                        s_detail.detail.device.online && home_allowed);
}

static void render_fan_speed(esp_gsp_handle_t ui, uint16_t endpoint)
{
    const matter_vm_property_t *property = detail_property(endpoint,
                                                             "fan_control", "percent");
    const matter_vm_action_t *action = detail_action(endpoint,
                                                     "fan_control", "set_percent");
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_SPEED_VALUE,
                           property ? property->display_value : "Unknown");
    if (property && property->value_kind == MATTER_VM_VALUE_NUMBER)
        (void)gsp_matter_matter_speed_slider_set_value(ui,
                                        (int32_t)lround(property->number_value));
    (void)esp_gsp_component_set_enabled(ui, GSP_OBJ_KEY_MATTER_SPEED_SLIDER,
        s_detail.detail.device.online && action && property &&
        property->value_kind == MATTER_VM_VALUE_NUMBER);
}

static void render_target_slider(esp_gsp_handle_t ui,
                                 const matter_state_row_t *selected)
{
    const matter_vm_property_t *property = detail_property(selected->endpoint,
                                                             "thermostat", selected->key);
    const matter_vm_action_t *action = detail_action(selected->endpoint,
        "thermostat", !strcmp(selected->key, "heating_setpoint_c")
                          ? "set_heating_setpoint" : "set_cooling_setpoint");
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_TARGET_VALUE,
                           property ? property->display_value : "Unknown");
    bool enabled = s_detail.detail.device.online && property && action &&
        property->value_kind == MATTER_VM_VALUE_NUMBER &&
        action->argument_count && action->arguments[0].has_min &&
        action->arguments[0].has_max &&
        action->arguments[0].max > action->arguments[0].min;
    if (enabled) {
        const matter_vm_argument_t *range = &action->arguments[0];
        double position = 100.0 * (property->number_value - range->min) /
                          (range->max - range->min);
        (void)gsp_matter_matter_target_slider_set_value(ui,
                               (int32_t)lround(fmax(0, fmin(100, position))));
    }
    (void)esp_gsp_component_set_enabled(ui, GSP_OBJ_KEY_MATTER_TARGET_SLIDER,
                                        enabled);
}

static void render_extended_controls(esp_gsp_handle_t ui,
                                      const matter_state_row_t *selected)
{
    matter_detail_control_t control = selected ? selected->control
                                                : MATTER_DETAIL_CONTROL_NONE;
    bool extended = control >= MATTER_DETAIL_CONTROL_MODE;
    if (!extended) return;
    uint16_t endpoint = selected->endpoint;
    bool is_fan = control == MATTER_DETAIL_CONTROL_FAN;
    bool is_rvc = control == MATTER_DETAIL_CONTROL_RVC_MODES;
    if (is_rvc || control == MATTER_DETAIL_CONTROL_RVC_COMMANDS)
        rvc_prepare_modes(endpoint);
    (void)esp_gsp_set_visible(ui, GSP_BIND_MATTER_MODE_FIRST_VISIBLE,
                              control == MATTER_DETAIL_CONTROL_MODE || is_fan || is_rvc);
    if (!is_rvc)
        (void)esp_gsp_set_visible(ui, GSP_BIND_MATTER_MODE_SECOND_VISIBLE, false);
    (void)esp_gsp_set_visible(ui, GSP_BIND_MATTER_SPEED_VISIBLE, is_fan);
    (void)esp_gsp_set_visible(ui, GSP_BIND_MATTER_TARGET_VISIBLE,
                              control == MATTER_DETAIL_CONTROL_TARGET);
    render_rvc_commands(ui, endpoint,
                        control == MATTER_DETAIL_CONTROL_RVC_COMMANDS);
    if (control == MATTER_DETAIL_CONTROL_MODE || is_fan || is_rvc) {
        const char *cluster = is_fan ? "fan_control" : is_rvc
            ? detail_property(endpoint, "rvc_run_mode", "mode")
                ? "rvc_run_mode" : "rvc_clean_mode" : "thermostat";
        show_mode_button(ui, endpoint, cluster, is_fan ? "Mode" : is_rvc
                             ? !strcmp(cluster, "rvc_run_mode") ? "Run" : "Clean"
                             : "Mode", GSP_BIND_MATTER_MODE_FIRST_VISIBLE,
                             GSP_BIND_MATTER_MODE_FIRST_TEXT,
                             GSP_OBJ_KEY_MATTER_MODE_FIRST_PREVIOUS,
                             GSP_OBJ_KEY_MATTER_MODE_FIRST_NEXT);
        if (is_rvc && !strcmp(cluster, "rvc_run_mode"))
            show_mode_button(ui, endpoint, "rvc_clean_mode", "Clean",
                             GSP_BIND_MATTER_MODE_SECOND_VISIBLE,
                             GSP_BIND_MATTER_MODE_SECOND_TEXT,
                             GSP_OBJ_KEY_MATTER_MODE_SECOND_PREVIOUS,
                             GSP_OBJ_KEY_MATTER_MODE_SECOND_NEXT);
    }
    if (is_fan) render_fan_speed(ui, endpoint);
    else if (control == MATTER_DETAIL_CONTROL_TARGET)
        render_target_slider(ui, selected);
}

static esp_gsp_err_t set_editor_visibility(esp_gsp_handle_t ui,
                                            bool editing,
                                            matter_detail_control_t control)
{
    const gsp_component_update_t updates[] = {
        {GSP_OBJ_KEY_MATTER_ENTRIES, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = !editing}},
        {GSP_OBJ_KEY_MATTER_EDITOR_PAGE, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = editing}},
        {GSP_OBJ_KEY_MATTER_POWER, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = control == MATTER_DETAIL_CONTROL_POWER}},
        {GSP_OBJ_KEY_MATTER_BRIGHTNESS, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = control == MATTER_DETAIL_CONTROL_BRIGHTNESS}},
        {GSP_OBJ_KEY_MATTER_COLOR_TEMPERATURE, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = control == MATTER_DETAIL_CONTROL_COLOR_TEMPERATURE}},
        {GSP_OBJ_KEY_MATTER_COLOR_CONTROLS, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = control == MATTER_DETAIL_CONTROL_HUE}},
        {GSP_OBJ_KEY_MATTER_READONLY, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = editing && control == MATTER_DETAIL_CONTROL_NONE}},
        {GSP_OBJ_KEY_MATTER_CONTROLS, GSP_COMPONENT_PROP_VISIBLE,
         {.type = GSP_VALUE_BOOL, .data.boolean = control >= MATTER_DETAIL_CONTROL_MODE}},
    };
    return esp_gsp_component_set_many(ui, updates,
                                       sizeof(updates) / sizeof(updates[0]));
}

static esp_err_t refresh_detail_list(esp_gsp_handle_t ui, bool editing)
{
    if (s_detail.detail_list == ESP_GSP_LIST_NONE) {
        s_detail.detail_list = esp_gsp_list_bind_component(
            ui, GSP_OBJ_KEY_MATTER_DETAIL_ROWS, bind_state_item, NULL);
    }
    if (s_detail.detail_list == ESP_GSP_LIST_NONE) return ESP_FAIL;
    if (s_detail.list_total != s_detail.state_row_count) {
        esp_gsp_err_t error = esp_gsp_list_set_total(
            ui, s_detail.detail_list, (uint32_t)s_detail.state_row_count);
        if (error != ESP_GSP_OK) return error;
        s_detail.list_total = s_detail.state_row_count;
    }
    if (!editing && s_detail.state_row_count) {
        esp_gsp_err_t error = esp_gsp_list_refresh(ui, s_detail.detail_list);
        if (error != ESP_GSP_OK) return error;
    }
    if (s_detail.detail_scroll_reset_pending && s_detail.state_row_count &&
        esp_gsp_list_scroll_to(ui, s_detail.detail_list, 0) == ESP_GSP_OK) {
        s_detail.detail_scroll_reset_pending = false;
    }
    return ESP_OK;
}

esp_err_t matter_detail_refresh(esp_gsp_handle_t ui)
{
    if (!ui || !s_detail.device_id) return ESP_ERR_INVALID_STATE;
    esp_err_t error = load_detail(ui);
    if (error != ESP_OK) return error;
    const matter_vm_device_t *device = &s_detail.detail.device;
    char status[MATTER_TEXT_MAX];
    snprintf(status, sizeof(status), "%s%s%s%s", detail_type(),
             device->room[0] ? " | " : "", device->room,
             device->online ? " | Online" : " | Offline");
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_DETAIL_NAME, device->name);
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_DETAIL_STATUS, status);
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_ACTION_STATUS,
                           s_detail.action_status);
    const matter_state_row_t *selected = s_detail.selected_entry <
        s_detail.state_row_count ? &s_detail.state_rows[s_detail.selected_entry]
                                 : NULL;
    if (selected) {
        (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_EDITOR_TITLE,
                               selected->name);
        (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_READONLY_VALUE,
                               selected->value);
    }
    const matter_detail_control_t control = selected
        ? selected->control : MATTER_DETAIL_CONTROL_NONE;
    error = set_editor_visibility(ui, selected != NULL, control);
    if (error != ESP_GSP_OK) return error;
    render_extended_controls(ui, selected);
    if (control == MATTER_DETAIL_CONTROL_POWER)
        matter_on_off_render(ui, &s_detail.on_off, device->online);
    if (control == MATTER_DETAIL_CONTROL_BRIGHTNESS)
        matter_level_render(ui, &s_detail.level, device->online);
    if (control == MATTER_DETAIL_CONTROL_COLOR_TEMPERATURE)
        matter_color_render_temperature(ui, &s_detail.color, device->online);
    if (control == MATTER_DETAIL_CONTROL_HUE)
        matter_color_render_controls(ui, &s_detail.color, device->online);
    return refresh_detail_list(ui, selected != NULL);
}

void matter_detail_property_changed(void)
{
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_detail.event_lock);
#endif
    ++s_detail.property_generation;
#if defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&s_detail.event_lock);
#endif
}

static bool rvc_start_state_reported(void)
{
    const matter_vm_property_t *state = detail_property(s_detail.rvc_draft_endpoint,
                                              "rvc_operational_state", "state");
    return state && state->value_kind == MATTER_VM_VALUE_STRING &&
           (!strcmp(state->string_value, "running") ||
            !strcmp(state->string_value, "paused") ||
            !strcmp(state->string_value, "seeking_charger"));
}

static void rvc_finish_start(esp_gsp_handle_t ui, esp_err_t error,
                              rvc_start_phase_t phase)
{
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_detail.event_lock);
#endif
    s_detail.rvc_start_phase = error == ESP_OK && phase == RVC_START_RUN
                               ? RVC_START_WAIT_STATE : RVC_START_NONE;
#if defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&s_detail.event_lock);
#endif
    snprintf(s_detail.action_status, sizeof(s_detail.action_status), "%s",
             error == ESP_OK ? "Syncing…" : "Command failed");
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_ACTION_STATUS,
                           s_detail.action_status);
}

static void process_rvc_start_events(esp_gsp_handle_t ui,
                                     bool completed, esp_err_t error,
                                     rvc_start_phase_t phase, bool reported)
{
    if (completed && error == ESP_OK && phase == RVC_START_CLEAN) {
        error = rvc_phase_is_idle(s_detail.rvc_draft_endpoint)
                    ? rvc_send_start_mode(ui, s_detail.rvc_draft_endpoint,
                                          RVC_START_RUN)
                    : ESP_ERR_INVALID_STATE;
        if (error == ESP_OK) {
            if (focused_control(MATTER_DETAIL_CONTROL_RVC_COMMANDS))
                render_rvc_commands(ui, s_detail.rvc_draft_endpoint, true);
            return;
        }
    }
    if (completed) rvc_finish_start(ui, error, phase);
    if (s_detail.rvc_start_phase == RVC_START_WAIT_STATE &&
        rvc_start_state_reported()) {
#if defined(ESP_PLATFORM)
        portENTER_CRITICAL(&s_detail.event_lock);
#endif
        s_detail.rvc_start_phase = RVC_START_NONE;
#if defined(ESP_PLATFORM)
        portEXIT_CRITICAL(&s_detail.event_lock);
#endif
        snprintf(s_detail.action_status, sizeof(s_detail.action_status), "Done");
        (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_ACTION_STATUS,
                               s_detail.action_status);
    }
    if (completed || reported) {
        if (focused_control(MATTER_DETAIL_CONTROL_RVC_COMMANDS))
            render_rvc_commands(ui, s_detail.rvc_draft_endpoint, true);
        else if (focused_control(MATTER_DETAIL_CONTROL_RVC_MODES))
            render_extended_controls(ui,
                                     &s_detail.state_rows[s_detail.selected_entry]);
    }
}

void matter_detail_process_events(esp_gsp_handle_t ui)
{
    bool completion = false;
    bool action_reported = false;
    esp_err_t completion_error = ESP_OK;
    bool start_completion = false;
    esp_err_t start_error = ESP_OK;
    rvc_start_phase_t start_phase = RVC_START_NONE;
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_detail.event_lock);
#endif
    completion = s_detail.completion_pending;
    completion_error = s_detail.completion_error;
    s_detail.completion_pending = false;
    start_completion = s_detail.rvc_start_completion;
    start_error = s_detail.rvc_start_error;
    start_phase = s_detail.rvc_start_phase;
    s_detail.rvc_start_completion = false;
    action_reported = s_detail.action_waiting_report &&
                      s_detail.property_generation !=
                          s_detail.action_property_generation;
    if (action_reported) s_detail.action_waiting_report = false;
#if defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&s_detail.event_lock);
#endif
    if (action_reported) {
        snprintf(s_detail.action_status, sizeof(s_detail.action_status),
                 "Done");
    }
    if (completion) {
        if (completion_error == ESP_OK) {
            snprintf(s_detail.action_status, sizeof(s_detail.action_status),
                     "%s", action_reported ? "Done" : "Syncing…");
        } else {
            snprintf(s_detail.action_status, sizeof(s_detail.action_status),
                     "Command failed");
#if defined(ESP_PLATFORM)
            portENTER_CRITICAL(&s_detail.event_lock);
#endif
            s_detail.action_waiting_report = false;
#if defined(ESP_PLATFORM)
            portEXIT_CRITICAL(&s_detail.event_lock);
#endif
        }
    }
    if (completion || action_reported) {
        (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_ACTION_STATUS,
                               s_detail.action_status);
        if (completion && completion_error != ESP_OK &&
            focused_control(MATTER_DETAIL_CONTROL_POWER)) {
            matter_on_off_render(ui, &s_detail.on_off,
                                 s_detail.detail.device.online);
        }
    }
    process_rvc_start_events(ui, start_completion, start_error,
                             start_phase, action_reported);
}

esp_err_t matter_detail_invoke_json(esp_gsp_handle_t ui,
                                     const matter_action_binding_t *binding,
                                     const char *arguments)
{
    if (!binding->available || !s_detail.device_id) return ESP_ERR_INVALID_STATE;
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_detail.event_lock);
#endif
    s_detail.action_property_generation = s_detail.property_generation;
    s_detail.action_waiting_report = true;
#if defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&s_detail.event_lock);
#endif
    fabric_core_request_id_t request_id = 0;
    const esp_err_t error = fabric_manager_invoke_action(
        s_detail.manager, s_detail.device_id, binding->value.endpoint,
        binding->value.cluster, binding->value.key, arguments, &request_id,
        action_complete, NULL);
    if (error == ESP_OK) {
        snprintf(s_detail.action_status, sizeof(s_detail.action_status),
                 "Request submitted");
    } else {
#if defined(ESP_PLATFORM)
        portENTER_CRITICAL(&s_detail.event_lock);
#endif
        s_detail.action_waiting_report = false;
#if defined(ESP_PLATFORM)
        portEXIT_CRITICAL(&s_detail.event_lock);
#endif
        snprintf(s_detail.action_status, sizeof(s_detail.action_status),
                 "Request failed");
    }
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_ACTION_STATUS,
                           s_detail.action_status);
    return error;
}

static bool focused_control(matter_detail_control_t control)
{
    return s_detail.selected_entry < s_detail.state_row_count &&
           s_detail.state_rows[s_detail.selected_entry].control == control;
}

static void stage_rvc_mode(esp_gsp_handle_t ui,
                           const matter_state_row_t *selected,
                           const char *cluster, bool second, int direction)
{
    rvc_prepare_modes(selected->endpoint);
    if (!rvc_modes_editable(selected->endpoint) || !s_detail.rvc_draft_ready)
        return;
    const matter_vm_action_t *action = detail_action(selected->endpoint,
                                                    cluster, "set_mode");
    if (!action) return;
    const matter_vm_argument_t *mode = &action->arguments[0];
    bool run = !strcmp(cluster, "rvc_run_mode");
    size_t *choice = run ? &s_detail.rvc_run_choice : &s_detail.rvc_clean_choice;
    if (*choice >= mode->enum_count || mode->enum_count < 2) return;
    size_t next = *choice;
    do {
        next = (next + mode->enum_count + direction) % mode->enum_count;
    } while (run && mode->enum_ids[next] == 0 && next != *choice);
    if (run && mode->enum_ids[next] == 0) return;
    *choice = next;
    char label[MATTER_VM_TEXT_MAX + 24];
    snprintf(label, sizeof(label), "%s: %.72s", run ? "Run" : "Clean",
             mode->enum_values[next]);
    (void)esp_gsp_set_text(ui,
        second ? GSP_BIND_MATTER_MODE_SECOND_TEXT :
                 GSP_BIND_MATTER_MODE_FIRST_TEXT, label);
}

void matter_detail_step_mode(esp_gsp_handle_t ui, bool second, int direction)
{
    if (!ui || !s_detail.detail.device.online ||
        s_detail.selected_entry >= s_detail.state_row_count ||
        (direction != -1 && direction != 1)) return;
    const matter_state_row_t *selected = &s_detail.state_rows[s_detail.selected_entry];
    if (second && (selected->control != MATTER_DETAIL_CONTROL_RVC_MODES ||
                   !detail_property(selected->endpoint, "rvc_run_mode", "mode")))
        return;
    const char *cluster = mode_cluster(selected, second);
    if (!cluster) return;
    if (selected->control == MATTER_DETAIL_CONTROL_RVC_MODES) {
        stage_rvc_mode(ui, selected, cluster, second, direction);
        return;
    }
    const matter_vm_action_t *action = detail_action(selected->endpoint,
                                                     cluster, "set_mode");
    const matter_vm_property_t *property = detail_property(
        selected->endpoint, cluster,
        !strcmp(cluster, "thermostat") ? "system_mode" : "mode");
    size_t current = 0;
    if (!mode_choice_index(action, property, &current) ||
        action->arguments[0].enum_count < 2) return;
    const matter_vm_argument_t *argument = &action->arguments[0];
    size_t next = (current + argument->enum_count + direction) % argument->enum_count;
    char arguments[MATTER_VM_TEXT_MAX + 32];
    if (argument->kind == MATTER_VM_ARGUMENT_INTEGER) {
        snprintf(arguments, sizeof(arguments), "{\"mode\":%ld}",
                 (long)argument->enum_ids[next]);
    } else {
        snprintf(arguments, sizeof(arguments), "{\"mode\":\"%s\"}",
                 argument->enum_values[next]);
    }
    matter_action_binding_t binding = {.available = true, .value = *action};
    (void)matter_detail_invoke_json(ui, &binding, arguments);
}

void matter_detail_set_target(esp_gsp_handle_t ui, int32_t position)
{
    if (!focused_control(MATTER_DETAIL_CONTROL_TARGET) ||
        !s_detail.detail.device.online) return;
    const matter_state_row_t *selected = &s_detail.state_rows[s_detail.selected_entry];
    const char *key = !strcmp(selected->key, "heating_setpoint_c")
                          ? "set_heating_setpoint" : "set_cooling_setpoint";
    const matter_vm_action_t *action = detail_action(selected->endpoint,
                                                      "thermostat", key);
    const matter_vm_property_t *property = detail_property(selected->endpoint,
                                                            "thermostat", selected->key);
    if (!action || action->argument_count != 1 || !property ||
        property->value_kind != MATTER_VM_VALUE_NUMBER ||
        !action->arguments[0].has_min || !action->arguments[0].has_max ||
        action->arguments[0].max <= action->arguments[0].min) return;
    if (position < 0) position = 0;
    if (position > 100) position = 100;
    double target = action->arguments[0].min + position / 100.0 *
        (action->arguments[0].max - action->arguments[0].min);
    char arguments[64];
    snprintf(arguments, sizeof(arguments), "{\"temperature_c\":%.2f}", target);
    matter_action_binding_t binding = {.available = true, .value = *action};
    (void)matter_detail_invoke_json(ui, &binding, arguments);
}

void matter_detail_set_speed(esp_gsp_handle_t ui, int32_t percent)
{
    if (!focused_control(MATTER_DETAIL_CONTROL_FAN) ||
        !s_detail.detail.device.online) return;
    const matter_state_row_t *selected = &s_detail.state_rows[s_detail.selected_entry];
    const matter_vm_action_t *action = detail_action(selected->endpoint,
                                                  "fan_control", "set_percent");
    const matter_vm_property_t *property = detail_property(selected->endpoint,
                                                         "fan_control", "percent");
    if (!action || !property || property->value_kind != MATTER_VM_VALUE_NUMBER) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    char arguments[40];
    snprintf(arguments, sizeof(arguments), "{\"percent\":%ld}", (long)percent);
    matter_action_binding_t binding = {.available = true, .value = *action};
    (void)matter_detail_invoke_json(ui, &binding, arguments);
}

static void invoke_rvc_command(esp_gsp_handle_t ui, const char *key)
{
    if (!focused_control(MATTER_DETAIL_CONTROL_RVC_COMMANDS) ||
        !s_detail.detail.device.online) return;
    const matter_state_row_t *selected = &s_detail.state_rows[s_detail.selected_entry];
    const matter_vm_action_t *action = detail_action(selected->endpoint,
                                                    "rvc_operational_state", key);
    if (!action) return;
    matter_action_binding_t binding = {.available = true, .value = *action};
    (void)matter_detail_invoke_json(ui, &binding, "{}");
}

void matter_detail_pause_resume(esp_gsp_handle_t ui)
{
    if (s_detail.rvc_start_phase != RVC_START_NONE ||
        s_detail.selected_entry >= s_detail.state_row_count) return;
    uint16_t endpoint = s_detail.state_rows[s_detail.selected_entry].endpoint;
    const matter_vm_property_t *state = detail_property(endpoint,
                                                "rvc_operational_state", "state");
    if (!state || state->value_kind != MATTER_VM_VALUE_STRING) return;
    if (!strcmp(state->string_value, "running")) invoke_rvc_command(ui, "pause");
    else if (!strcmp(state->string_value, "paused")) invoke_rvc_command(ui, "resume");
    else if (focused_control(MATTER_DETAIL_CONTROL_RVC_COMMANDS) &&
             rvc_modes_editable(endpoint)) {
        rvc_prepare_modes(endpoint);
        if (!s_detail.rvc_draft_ready) return;
#if defined(ESP_PLATFORM)
        portENTER_CRITICAL(&s_detail.event_lock);
#endif
        ++s_detail.rvc_start_generation;
#if defined(ESP_PLATFORM)
        portEXIT_CRITICAL(&s_detail.event_lock);
#endif
        (void)rvc_send_start_mode(ui, endpoint, RVC_START_CLEAN);
        render_rvc_commands(ui, endpoint, true);
    }
}

void matter_detail_go_home(esp_gsp_handle_t ui)
{
    if (!focused_control(MATTER_DETAIL_CONTROL_RVC_COMMANDS) ||
        s_detail.rvc_start_phase != RVC_START_NONE ||
        !rvc_can_go_home(s_detail.state_rows[s_detail.selected_entry].endpoint))
        return;
    invoke_rvc_command(ui, "go_home");
}

void matter_detail_toggle_power(esp_gsp_handle_t ui)
{
    if (!focused_control(MATTER_DETAIL_CONTROL_POWER)) return;
    if (matter_on_off_toggle(ui, &s_detail.on_off,
                             s_detail.detail.device.online) != ESP_OK) {
        matter_on_off_render(ui, &s_detail.on_off,
                             s_detail.detail.device.online);
    }
}

void matter_detail_set_level(esp_gsp_handle_t ui, int32_t percent)
{
    if (!focused_control(MATTER_DETAIL_CONTROL_BRIGHTNESS)) return;
    matter_level_set(ui, &s_detail.level, s_detail.detail.device.online, percent);
}

void matter_detail_set_color(esp_gsp_handle_t ui, int32_t hue_degrees,
                             int32_t saturation_percent)
{
    if (!focused_control(MATTER_DETAIL_CONTROL_HUE)) return;
    matter_color_set(ui, &s_detail.color, s_detail.detail.device.online,
                     hue_degrees, saturation_percent);
}

void matter_detail_set_hue(esp_gsp_handle_t ui, int32_t hue_degrees)
{
    if (!focused_control(MATTER_DETAIL_CONTROL_HUE)) return;
    matter_color_set_hue(ui, &s_detail.color, s_detail.detail.device.online,
                         hue_degrees);
}

void matter_detail_set_saturation(esp_gsp_handle_t ui,
                                  int32_t saturation_percent)
{
    if (!focused_control(MATTER_DETAIL_CONTROL_HUE)) return;
    matter_color_set_saturation(ui, &s_detail.color, s_detail.detail.device.online,
                                saturation_percent);
}

void matter_detail_set_color_temperature(esp_gsp_handle_t ui, int32_t percent)
{
    if (!focused_control(MATTER_DETAIL_CONTROL_COLOR_TEMPERATURE)) return;
    matter_color_set_temperature(ui, &s_detail.color,
                                 s_detail.detail.device.online, percent);
}

/* On/Off state must be reported before Toggle can invert it safely. */
void matter_on_off_update(matter_on_off_detail_t *state,
                          const matter_vm_detail_t *detail)
{
    matter_detail_bind_action(&state->action, detail, "on_off", "set");
    const matter_vm_property_t *property = matter_detail_bound_property(
        detail, &state->action, "on");
    state->known = property &&
                   property->value_kind == MATTER_VM_VALUE_BOOLEAN;
    state->on = state->known && property->boolean_value;
}

void matter_on_off_render(esp_gsp_handle_t ui,
                          const matter_on_off_detail_t *state, bool online)
{
    if (!state->action.available) return;
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_POWER_STATE,
                           state->known ? (state->on ? "On" : "Off")
                                        : "Unknown");
    (void)gsp_matter_matter_power_toggle_set_checked(ui, state->on);
    (void)gsp_matter_matter_power_toggle_set_track_color_rgb888(
        ui, online && state->known && state->on
                ? MATTER_ACCENT_RGB888 : MATTER_TOGGLE_OFF_COLOR);
    (void)esp_gsp_component_set_enabled(ui, GSP_OBJ_KEY_MATTER_POWER_TOGGLE,
                                        online && state->known);
    (void)esp_gsp_component_set_enabled(ui, GSP_OBJ_KEY_MATTER_POWER_TAP,
                                        online && state->known);
}

static esp_err_t matter_on_off_toggle(esp_gsp_handle_t ui,
                                       const matter_on_off_detail_t *state,
                                       bool online)
{
    if (!state->action.available || !online || !state->known) {
        return ESP_ERR_INVALID_STATE;
    }
    return matter_detail_invoke_json(ui, &state->action,
                                     state->on ? "{\"on\":false}"
                                               : "{\"on\":true}");
}

/* Level commands are bounded by the action schema's 1..100 percent range. */
void matter_level_update(matter_level_detail_t *state,
                         const matter_vm_detail_t *detail)
{
    matter_detail_bind_action(&state->action, detail, "level_control",
                              "set_level");
    const matter_vm_property_t *property = matter_detail_bound_property(
        detail, &state->action, "level_percent");
    state->known = property &&
                   property->value_kind == MATTER_VM_VALUE_NUMBER &&
                   property->number_value >= 1 &&
                   property->number_value <= 100;
    state->percent = state->known ? (int32_t)lround(property->number_value) : 0;
}

void matter_level_render(esp_gsp_handle_t ui,
                         const matter_level_detail_t *state, bool online)
{
    if (!state->action.available) return;
    char text[16] = "Unknown";
    if (state->known) {
        snprintf(text, sizeof(text), "%ld%%", (long)state->percent);
        (void)gsp_matter_matter_brightness_slider_set_value(ui, state->percent);
    }
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIGHTNESS_STATE, text);
    (void)esp_gsp_component_set_enabled(
        ui, GSP_OBJ_KEY_MATTER_BRIGHTNESS_SLIDER, online && state->known);
}

void matter_level_set(esp_gsp_handle_t ui, const matter_level_detail_t *state,
                      bool online, int32_t percent)
{
    if (!state->action.available || !online || !state->known) return;
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    char arguments[48];
    snprintf(arguments, sizeof(arguments), "{\"level_percent\":%ld}",
             (long)percent);
    matter_detail_invoke_json(ui, &state->action, arguments);
}

/* Color temperature is Kelvin at the UI boundary; CHIP uses mireds below Manager. */
static int32_t clamp_i32(int32_t value, int32_t minimum, int32_t maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static const matter_vm_argument_t *temperature_argument(
    const matter_vm_action_t *action)
{
    for (size_t index = 0; index < action->argument_count; ++index) {
        if (!strcmp(action->arguments[index].key,
                    "color_temperature_kelvin")) {
            return &action->arguments[index];
        }
    }
    return NULL;
}

static int32_t reported_number(const matter_vm_property_t *property,
                                int32_t fallback)
{
    return property && property->value_kind == MATTER_VM_VALUE_NUMBER &&
                   property->number_value >= INT32_MIN &&
                   property->number_value <= INT32_MAX
               ? (int32_t)lround(property->number_value)
               : fallback;
}

matter_detail_control_t matter_color_property_control(
    const matter_vm_property_t *property)
{
    if (strcmp(property->cluster, "color_control")) {
        return MATTER_DETAIL_CONTROL_NONE;
    }
    if (!strcmp(property->key, "color_temperature_kelvin")) {
        return MATTER_DETAIL_CONTROL_COLOR_TEMPERATURE;
    }
    if (!strcmp(property->key, "hue_degrees")) {
        return MATTER_DETAIL_CONTROL_HUE;
    }
    if (!strcmp(property->key, "saturation_percent")) {
        return MATTER_DETAIL_CONTROL_SATURATION;
    }
    return MATTER_DETAIL_CONTROL_NONE;
}

static bool matter_color_has_pair(const matter_vm_detail_t *detail,
                                   const matter_color_detail_t *state)
{
    const matter_vm_property_t *hue = matter_detail_bound_property(
        detail, &state->color_action, "hue_degrees");
    const matter_vm_property_t *saturation = matter_detail_bound_property(
        detail, &state->color_action, "saturation_percent");
    return hue && saturation &&
           matter_detail_action_matches(&state->color_action, hue,
                                         "hue_degrees") &&
           matter_detail_action_matches(&state->color_action, saturation,
                                         "saturation_percent");
}

static const char *matter_color_row_label(const matter_vm_property_t *property)
{
    switch (matter_color_property_control(property)) {
    case MATTER_DETAIL_CONTROL_HUE: return "Hue";
    case MATTER_DETAIL_CONTROL_SATURATION: return "Saturation";
    case MATTER_DETAIL_CONTROL_COLOR_TEMPERATURE: return "Color temperature";
    default: return NULL;
    }
}

static void matter_color_row_value(const matter_vm_property_t *property,
                                    char *text, size_t capacity)
{
    const matter_detail_control_t control =
        matter_color_property_control(property);
    if (property->value_kind == MATTER_VM_VALUE_NUMBER &&
        property->number_value >= 0 && property->number_value <= INT32_MAX) {
        const long value = lround(property->number_value);
        if (control == MATTER_DETAIL_CONTROL_HUE) {
            snprintf(text, capacity, "%ld°", value);
            return;
        }
        if (control == MATTER_DETAIL_CONTROL_SATURATION) {
            snprintf(text, capacity, "%ld%%", value);
            return;
        }
        if (control == MATTER_DETAIL_CONTROL_COLOR_TEMPERATURE) {
            snprintf(text, capacity, "%ld K", value);
            return;
        }
    }
    snprintf(text, capacity, "%s", property->display_value);
}

void matter_color_update(matter_color_detail_t *state,
                         const matter_vm_detail_t *detail)
{
    matter_detail_bind_action(&state->color_action, detail, "color_control",
                              "set_color");
    matter_detail_bind_action(&state->temperature_action, detail,
                              "color_control", "set_color_temperature");
    const matter_vm_property_t *property = matter_detail_bound_property(
        detail, &state->color_action, "hue_degrees");
    state->hue_known = property &&
        property->value_kind == MATTER_VM_VALUE_NUMBER &&
        property->number_value >= 0 && property->number_value <= 359;
    state->hue_degrees = state->hue_known ? reported_number(property, 0) : 0;
    property = matter_detail_bound_property(detail, &state->color_action,
                                             "saturation_percent");
    state->saturation_known = property &&
        property->value_kind == MATTER_VM_VALUE_NUMBER &&
        property->number_value >= 0 && property->number_value <= 100;
    state->saturation_percent = state->saturation_known
        ? reported_number(property, 0) : 0;

    const matter_vm_argument_t *argument = state->temperature_action.available
        ? temperature_argument(&state->temperature_action.value) : NULL;
    state->min_kelvin = argument && argument->has_min &&
                                 argument->min >= TEMPERATURE_MIN_K &&
                                 argument->min <= TEMPERATURE_MAX_K
        ? (int32_t)lround(argument->min) : TEMPERATURE_MIN_K;
    state->max_kelvin = argument && argument->has_max &&
                                 argument->max >= TEMPERATURE_MIN_K &&
                                 argument->max <= TEMPERATURE_MAX_K
        ? (int32_t)lround(argument->max) : TEMPERATURE_MAX_K;
    if (state->max_kelvin <= state->min_kelvin) {
        state->min_kelvin = TEMPERATURE_MIN_K;
        state->max_kelvin = TEMPERATURE_MAX_K;
    }
    property = matter_detail_bound_property(detail,
                                             &state->temperature_action,
                                             "color_temperature_kelvin");
    state->temperature_known = property &&
        property->value_kind == MATTER_VM_VALUE_NUMBER &&
        property->number_value >= 1 && property->number_value <= INT32_MAX;
    state->temperature_kelvin = reported_number(property, state->min_kelvin);
}

void matter_color_render_temperature(esp_gsp_handle_t ui,
                                     const matter_color_detail_t *state,
                                     bool online)
{
    if (!state->inline_temperature) return;
    char text[32] = "Unknown";
    if (state->temperature_known) {
        snprintf(text, sizeof(text), "%ld K", (long)state->temperature_kelvin);
        const int32_t kelvin = clamp_i32(state->temperature_kelvin,
                                          state->min_kelvin, state->max_kelvin);
        const int32_t percent = (int32_t)(
            (int64_t)(kelvin - state->min_kelvin) * 100 /
            (state->max_kelvin - state->min_kelvin));
        (void)gsp_matter_matter_color_temperature_slider_set_value(ui,
                                                                    percent);
    }
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_COLOR_TEMPERATURE_STATE, text);
    snprintf(text, sizeof(text), "Warm %ld K", (long)state->min_kelvin);
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_TEMPERATURE_WARM, text);
    snprintf(text, sizeof(text), "Cool %ld K", (long)state->max_kelvin);
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_TEMPERATURE_COOL, text);
    (void)esp_gsp_component_set_enabled(
        ui, GSP_OBJ_KEY_MATTER_COLOR_TEMPERATURE_SLIDER,
        online && state->temperature_known);
}

void matter_color_render_hue_editor(esp_gsp_handle_t ui, int32_t degrees)
{
    degrees = clamp_i32(degrees, 0, 359);
    char text[16];
    snprintf(text, sizeof(text), "%ld°", (long)degrees);
    (void)gsp_matter_matter_hue_slider_set_value(ui, degrees);
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_HUE_TEXT, text);
}

void matter_color_render_saturation_editor(esp_gsp_handle_t ui,
                                           int32_t percent)
{
    percent = clamp_i32(percent, 0, 100);
    char text[16];
    snprintf(text, sizeof(text), "%ld%%", (long)percent);
    (void)gsp_matter_matter_saturation_slider_set_value(ui, percent);
    (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_SATURATION_TEXT, text);
}

void matter_color_render_controls(esp_gsp_handle_t ui,
                                  const matter_color_detail_t *state,
                                  bool online)
{
    if (!state->inline_color) return;
    matter_color_render_hue_editor(ui, state->hue_degrees);
    matter_color_render_saturation_editor(ui, state->saturation_percent);
    if (!state->hue_known) {
        (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_HUE_TEXT, "Unknown");
    }
    if (!state->saturation_known) {
        (void)esp_gsp_set_text(ui, GSP_BIND_MATTER_SATURATION_TEXT,
                               "Unknown");
    }
    const bool enabled = online && state->hue_known &&
                         state->saturation_known;
    (void)esp_gsp_component_set_enabled(ui, GSP_OBJ_KEY_MATTER_HUE_SLIDER,
                                        enabled);
    (void)esp_gsp_component_set_enabled(
        ui, GSP_OBJ_KEY_MATTER_SATURATION_SLIDER, enabled);
}

void matter_color_set(esp_gsp_handle_t ui, matter_color_detail_t *state,
                      bool online, int32_t hue, int32_t saturation)
{
    if (!state->inline_color || !state->color_action.available || !online ||
        !state->hue_known || !state->saturation_known) return;
    hue = clamp_i32(hue, 0, 359);
    saturation = clamp_i32(saturation, 0, 100);
    state->hue_degrees = hue;
    state->saturation_percent = saturation;
    char arguments[96];
    snprintf(arguments, sizeof(arguments),
             "{\"hue_degrees\":%ld,\"saturation_percent\":%ld}",
             (long)hue, (long)saturation);
    matter_detail_invoke_json(ui, &state->color_action, arguments);
}

void matter_color_set_hue(esp_gsp_handle_t ui, matter_color_detail_t *state,
                          bool online, int32_t hue)
{
    if (!state->inline_color || !online || !state->hue_known ||
        !state->saturation_known) return;
    matter_color_render_hue_editor(ui, hue);
    matter_color_set(ui, state, online, hue, state->saturation_percent);
}

void matter_color_set_saturation(esp_gsp_handle_t ui,
                                 matter_color_detail_t *state, bool online,
                                 int32_t saturation)
{
    if (!state->inline_color || !online || !state->hue_known ||
        !state->saturation_known) return;
    matter_color_render_saturation_editor(ui, saturation);
    matter_color_set(ui, state, online, state->hue_degrees, saturation);
}

void matter_color_set_temperature(esp_gsp_handle_t ui,
                                  const matter_color_detail_t *state,
                                  bool online, int32_t percent)
{
    if (!state->inline_temperature || !state->temperature_action.available ||
        !online || !state->temperature_known) return;
    percent = clamp_i32(percent, 0, 100);
    const int32_t kelvin = state->min_kelvin +
        (int32_t)((int64_t)percent * (state->max_kelvin - state->min_kelvin)
                  / 100);
    char arguments[72];
    snprintf(arguments, sizeof(arguments),
             "{\"color_temperature_kelvin\":%ld}", (long)kelvin);
    matter_detail_invoke_json(ui, &state->temperature_action, arguments);
}
