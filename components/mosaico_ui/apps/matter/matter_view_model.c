/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "matter_view_model.h"

#include "cJSON.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_text(char *output, size_t capacity, const char *value)
{
    if (!output || capacity == 0) return;
    snprintf(output, capacity, "%s", value ? value : "");
}

static const char *json_string(const cJSON *object, const char *key,
                               const char *fallback)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring
                                                       : fallback;
}

static bool json_device_id(const cJSON *object, fabric_manager_device_id_t *output)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, "device_id");
    if (!cJSON_IsNumber(value) || value->valuedouble < 1 ||
        value->valuedouble > UINT32_MAX ||
        floor(value->valuedouble) != value->valuedouble) {
        return false;
    }
    *output = (fabric_manager_device_id_t)value->valuedouble;
    return true;
}

static bool parse_endpoint_id(const char *text, uint16_t *output)
{
    if (!text || !text[0]) return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (!end || *end || value > UINT16_MAX) return false;
    *output = (uint16_t)value;
    return true;
}

static bool parse_device(const cJSON *input, matter_vm_device_t *output)
{
    if (!cJSON_IsObject(input) || !json_device_id(input, &output->id)) {
        return false;
    }
    copy_text(output->type, sizeof(output->type),
              json_string(input, "type", "device"));
    copy_text(output->name, sizeof(output->name),
              json_string(input, "name", "Matter device"));
    copy_text(output->room, sizeof(output->room),
              json_string(input, "room", ""));
    output->online = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
        input, "online"));
    return true;
}

esp_err_t matter_vm_parse_devices(const char *json, matter_vm_device_t *devices,
                                  size_t capacity, size_t *out_count)
{
    if (!json || (!devices && capacity) || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    cJSON *document = cJSON_Parse(json);
    const cJSON *items = document
                             ? cJSON_GetObjectItemCaseSensitive(document,
                                                                "devices")
                             : NULL;
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(document);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (count >= capacity) {
            cJSON_Delete(document);
            return ESP_ERR_INVALID_SIZE;
        }
        if (!parse_device(item, &devices[count])) {
            cJSON_Delete(document);
            return ESP_ERR_INVALID_RESPONSE;
        }
        ++count;
    }
    cJSON_Delete(document);
    *out_count = count;
    return ESP_OK;
}

static void format_key(const char *key, char *output, size_t capacity)
{
    size_t written = 0;
    bool capitalize = true;
    for (const char *cursor = key; cursor && *cursor && written + 1 < capacity;
         ++cursor) {
        if (*cursor == '_') {
            output[written++] = ' ';
            capitalize = true;
            continue;
        }
        output[written++] = capitalize ? (char)toupper((unsigned char)*cursor)
                                       : *cursor;
        capitalize = false;
    }
    output[written] = '\0';
}

static void parse_property_value(const cJSON *input,
                                 matter_vm_property_t *property)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(input, "value");
    copy_text(property->unit, sizeof(property->unit),
              json_string(input, "unit", ""));
    if (cJSON_IsNull(value) || !value) {
        property->value_kind = MATTER_VM_VALUE_NULL;
        copy_text(property->display_value, sizeof(property->display_value),
                  "Waiting for report");
    } else if (cJSON_IsBool(value)) {
        property->value_kind = MATTER_VM_VALUE_BOOLEAN;
        property->boolean_value = cJSON_IsTrue(value);
        const bool occupied = !strcmp(property->cluster, "occupancy_sensing") &&
                              !strcmp(property->key, "occupied");
        copy_text(property->display_value, sizeof(property->display_value),
                  occupied ? (property->boolean_value ? "Occupied" : "Unoccupied")
                           : (property->boolean_value ? "On" : "Off"));
    } else if (cJSON_IsNumber(value)) {
        property->value_kind = MATTER_VM_VALUE_NUMBER;
        property->number_value = value->valuedouble;
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(input, "label");
        if (cJSON_IsString(label)) {
            copy_text(property->display_value, sizeof(property->display_value),
                      label->valuestring);
            return;
        }
        const char *unit = property->unit;
        if (!strcmp(unit, "percent")) unit = "%";
        else if (!strcmp(unit, "celsius")) unit = "°C";
        else if (!strcmp(unit, "kelvin")) unit = "K";
        else if (!strcmp(unit, "degrees")) unit = "°";
        snprintf(property->display_value, sizeof(property->display_value),
                 "%g%s%s", property->number_value,
                 unit[0] && strcmp(unit, "%") && strcmp(unit, "°") ? " " : "",
                 unit);
    } else if (cJSON_IsString(value)) {
        property->value_kind = MATTER_VM_VALUE_STRING;
        copy_text(property->string_value, sizeof(property->string_value),
                  value->valuestring);
        copy_text(property->display_value, sizeof(property->display_value),
                  property->string_value);
        if (property->unit[0]) {
            size_t length = strlen(property->display_value);
            snprintf(property->display_value + length,
                     sizeof(property->display_value) - length, " %.48s",
                     property->unit);
        }
    } else {
        property->value_kind = MATTER_VM_VALUE_OTHER;
        char *encoded = cJSON_PrintUnformatted(value);
        copy_text(property->display_value, sizeof(property->display_value),
                  encoded ? encoded : "Unavailable");
        cJSON_free(encoded);
    }
}

static void property_name(const char *cluster, const char *key,
                          char *output, size_t capacity)
{
    static const struct {
        const char *cluster;
        const char *key;
        const char *label;
    } labels[] = {
        {"on_off", "on", "Power"},
        {"temperature_measurement", "temperature_celsius", "Temperature"},
        {"relative_humidity_measurement", "humidity_percent", "Humidity"},
        {"illuminance_measurement", "illuminance_lux", "Illuminance"},
        {"occupancy_sensing", "occupied", "Occupancy"},
        {"boolean_state", "state", "State"},
        {"power_source", "battery_percent", "Battery"},
    };
    for (size_t index = 0; index < sizeof(labels) / sizeof(labels[0]); ++index) {
        if (!strcmp(cluster, labels[index].cluster) &&
            !strcmp(key, labels[index].key)) {
            copy_text(output, capacity, labels[index].label);
            return;
        }
    }
    format_key(key, output, capacity);
}

static esp_err_t parse_properties(const cJSON *properties, uint16_t endpoint,
                                  const char *endpoint_name, const char *cluster,
                                  matter_vm_detail_t *detail, size_t *index)
{
    const cJSON *input = NULL;
    cJSON_ArrayForEach(input, properties) {
        if (*index >= detail->property_count || !input->string) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        matter_vm_property_t *property = &detail->properties[(*index)++];
        property->endpoint = endpoint;
        copy_text(property->endpoint_name, sizeof(property->endpoint_name),
                  endpoint_name);
        copy_text(property->cluster, sizeof(property->cluster), cluster);
        copy_text(property->key, sizeof(property->key), input->string);
        property_name(cluster, input->string, property->name,
                      sizeof(property->name));
        parse_property_value(input, property);
    }
    return ESP_OK;
}

static matter_vm_argument_kind_t parse_argument_kind(const char *type)
{
    if (!strcmp(type, "boolean")) return MATTER_VM_ARGUMENT_BOOLEAN;
    if (!strcmp(type, "integer")) return MATTER_VM_ARGUMENT_INTEGER;
    if (!strcmp(type, "string")) return MATTER_VM_ARGUMENT_STRING;
    return MATTER_VM_ARGUMENT_NUMBER;
}

static void parse_argument(const cJSON *input, matter_vm_argument_t *argument)
{
    copy_text(argument->key, sizeof(argument->key), input->string);
    argument->kind = parse_argument_kind(json_string(input, "type", "number"));
    const cJSON *minimum = cJSON_GetObjectItemCaseSensitive(input, "min");
    const cJSON *maximum = cJSON_GetObjectItemCaseSensitive(input, "max");
    argument->has_min = cJSON_IsNumber(minimum);
    argument->has_max = cJSON_IsNumber(maximum);
    argument->min = argument->has_min ? minimum->valuedouble : 0;
    argument->max = argument->has_max ? maximum->valuedouble : 0;

    const cJSON *values = cJSON_GetObjectItemCaseSensitive(input, "enum");
    const cJSON *value = NULL;
    cJSON_ArrayForEach(value, values) {
        if (argument->enum_count >= MATTER_VM_ENUM_MAX) break;
        if (cJSON_IsString(value)) {
            copy_text(argument->enum_values[argument->enum_count++],
                      MATTER_VM_TEXT_MAX, value->valuestring);
            continue;
        }
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(value, "value");
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(value, "label");
        if (cJSON_IsNumber(id) && cJSON_IsString(label) &&
            id->valuedouble >= 0 && id->valuedouble <= 255 &&
            id->valuedouble == floor(id->valuedouble)) {
            size_t index = argument->enum_count++;
            argument->enum_ids[index] = id->valueint;
            copy_text(argument->enum_values[index], MATTER_VM_TEXT_MAX,
                      label->valuestring);
        }
    }
}

static esp_err_t parse_actions(const cJSON *actions, uint16_t endpoint,
                               const char *cluster, matter_vm_detail_t *detail,
                               size_t *index)
{
    const cJSON *input = NULL;
    cJSON_ArrayForEach(input, actions) {
        if (*index >= detail->action_count || !input->string) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        matter_vm_action_t *action = &detail->actions[(*index)++];
        action->endpoint = endpoint;
        copy_text(action->cluster, sizeof(action->cluster), cluster);
        copy_text(action->key, sizeof(action->key), input->string);
        const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(input,
                                                                  "arguments");
        if (!cJSON_IsObject(arguments) ||
            cJSON_GetArraySize(arguments) > MATTER_VM_ACTION_ARGUMENT_MAX) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const cJSON *argument = NULL;
        cJSON_ArrayForEach(argument, arguments) {
            parse_argument(argument,
                           &action->arguments[action->argument_count++]);
        }
    }
    return ESP_OK;
}

static void count_members(const cJSON *endpoints, size_t *property_count,
                          size_t *action_count)
{
    const cJSON *endpoint = NULL;
    cJSON_ArrayForEach(endpoint, endpoints) {
        const cJSON *clusters = cJSON_GetObjectItemCaseSensitive(endpoint,
                                                                 "clusters");
        const cJSON *cluster = NULL;
        cJSON_ArrayForEach(cluster, clusters) {
            const cJSON *properties = cJSON_GetObjectItemCaseSensitive(
                cluster, "properties");
            const cJSON *actions = cJSON_GetObjectItemCaseSensitive(cluster,
                                                                    "actions");
            *property_count += cJSON_IsObject(properties)
                                   ? cJSON_GetArraySize(properties)
                                   : 0;
            *action_count += cJSON_IsObject(actions)
                                 ? cJSON_GetArraySize(actions)
                                 : 0;
        }
    }
}

static esp_err_t parse_members(const cJSON *endpoints,
                               matter_vm_detail_t *detail)
{
    size_t property_index = 0;
    size_t action_index = 0;
    const cJSON *endpoint = NULL;
    cJSON_ArrayForEach(endpoint, endpoints) {
        uint16_t endpoint_id = 0;
        if (!parse_endpoint_id(endpoint->string, &endpoint_id)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const cJSON *clusters = cJSON_GetObjectItemCaseSensitive(endpoint,
                                                                 "clusters");
        const cJSON *cluster = NULL;
        cJSON_ArrayForEach(cluster, clusters) {
            if (!cluster->string) return ESP_ERR_INVALID_RESPONSE;
            esp_err_t error = parse_properties(
                cJSON_GetObjectItemCaseSensitive(cluster, "properties"),
                endpoint_id, json_string(endpoint, "name", ""),
                cluster->string, detail, &property_index);
            if (error != ESP_OK) return error;
            error = parse_actions(
                cJSON_GetObjectItemCaseSensitive(cluster, "actions"),
                endpoint_id, cluster->string, detail, &action_index);
            if (error != ESP_OK) return error;
        }
    }
    return ESP_OK;
}

esp_err_t matter_vm_parse_detail(const char *json,
                                 matter_vm_detail_t *out_detail)
{
    if (!json || !out_detail) return ESP_ERR_INVALID_ARG;
    memset(out_detail, 0, sizeof(*out_detail));
    cJSON *document = cJSON_Parse(json);
    const cJSON *endpoints = document
                                 ? cJSON_GetObjectItemCaseSensitive(document,
                                                                    "endpoints")
                                 : NULL;
    if (!document || !cJSON_IsObject(endpoints) ||
        !parse_device(document, &out_detail->device)) {
        cJSON_Delete(document);
        return ESP_ERR_INVALID_RESPONSE;
    }

    count_members(endpoints, &out_detail->property_count,
                  &out_detail->action_count);
    out_detail->properties = calloc(out_detail->property_count,
                                    sizeof(*out_detail->properties));
    out_detail->actions = calloc(out_detail->action_count,
                                 sizeof(*out_detail->actions));
    if ((out_detail->property_count && !out_detail->properties) ||
        (out_detail->action_count && !out_detail->actions)) {
        cJSON_Delete(document);
        matter_vm_free_detail(out_detail);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = parse_members(endpoints, out_detail);
    cJSON_Delete(document);
    if (error != ESP_OK) matter_vm_free_detail(out_detail);
    return error;
}

void matter_vm_free_detail(matter_vm_detail_t *detail)
{
    if (!detail) return;
    free(detail->properties);
    free(detail->actions);
    memset(detail, 0, sizeof(*detail));
}

const matter_vm_property_t *matter_vm_find_property(
    const matter_vm_detail_t *detail, uint16_t endpoint, const char *cluster,
    const char *key)
{
    if (!detail || !cluster || !key) return NULL;
    for (size_t index = 0; index < detail->property_count; ++index) {
        const matter_vm_property_t *property = &detail->properties[index];
        if (property->endpoint == endpoint &&
            !strcmp(property->cluster, cluster) &&
            !strcmp(property->key, key)) {
            return property;
        }
    }
    return NULL;
}

const matter_vm_action_t *matter_vm_find_action(
    const matter_vm_detail_t *detail, const char *cluster, const char *key)
{
    if (!detail || !cluster || !key) return NULL;
    for (size_t index = 0; index < detail->action_count; ++index) {
        const matter_vm_action_t *action = &detail->actions[index];
        if (!strcmp(action->cluster, cluster) && !strcmp(action->key, key)) {
            return action;
        }
    }
    return NULL;
}
