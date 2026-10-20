/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_manager_private.h"

#include <cmath>
#include <cstdio>
#include <memory>

namespace data_model::json {
namespace {

struct cjson_delete {
    void operator()(cJSON *value) const { cJSON_Delete(value); }
};
using json_owner = std::unique_ptr<cJSON, cjson_delete>;

bool add_owned(cJSON *object, const char *key, json_owner value)
{
    if (!object || !key || !value ||
        !cJSON_AddItemToObject(object, key, value.get())) {
        return false;
    }
    (void)value.release();
    return true;
}

json_owner argument(const char *type)
{
    json_owner value(cJSON_CreateObject());
    if (!value ||
        !add_owned(value.get(), "type",
                   json_owner(cJSON_CreateString(type)))) {
        return {};
    }
    return value;
}

json_owner numeric_argument(const char *type, double minimum,
                            double maximum, const char *unit)
{
    auto value = argument(type);
    if (!value ||
        !add_owned(value.get(), "min",
                   json_owner(cJSON_CreateNumber(minimum))) ||
        !add_owned(value.get(), "max",
                   json_owner(cJSON_CreateNumber(maximum))) ||
        (unit &&
         !add_owned(value.get(), "unit",
                    json_owner(cJSON_CreateString(unit))))) {
        return {};
    }
    return value;
}

cJSON *summary(const device &value)
{
    cJSON *object = cJSON_CreateObject();
    if (!object ||
        !cJSON_AddNumberToObject(object, "device_id", value.id) ||
        !cJSON_AddStringToObject(object, "type", value.type) ||
        !cJSON_AddStringToObject(object, "name", value.name) ||
        !cJSON_AddStringToObject(object, "room", value.room) ||
        !cJSON_AddBoolToObject(object, "online", value.online)) {
        cJSON_Delete(object);
        return nullptr;
    }
    return object;
}

cJSON *encode(const cluster &value)
{
    cJSON *object = cJSON_CreateObject();
    cJSON *properties = object
                            ? cJSON_AddObjectToObject(object, "properties")
                            : nullptr;
    cJSON *actions =
        object ? cJSON_AddObjectToObject(object, "actions") : nullptr;
    if (!properties || !actions ||
        value.adapter->encode_json(&value.state, properties, actions) !=
            ESP_OK) {
        cJSON_Delete(object);
        return nullptr;
    }
    return object;
}

} // namespace

char *list(const device_set &set)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *devices = root ? cJSON_AddArrayToObject(root, "devices") : nullptr;
    if (!devices) {
        cJSON_Delete(root);
        return nullptr;
    }
    for (size_t index = 0; index < set.device_count; ++index) {
        cJSON *item = summary(set.devices[index]);
        if (!item || !cJSON_AddItemToArray(devices, item)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            return nullptr;
        }
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

char *detail(const device_set &set, const device &value)
{
    cJSON *root = summary(value);
    cJSON *endpoints =
        root ? cJSON_AddObjectToObject(root, "endpoints") : nullptr;
    if (!endpoints) {
        cJSON_Delete(root);
        return nullptr;
    }
    for (size_t endpoint_offset = 0; endpoint_offset < value.endpoint_count;
         ++endpoint_offset) {
        const auto &endpoint =
            set.endpoints[value.first_endpoint + endpoint_offset];
        char endpoint_key[8] = {};
        snprintf(endpoint_key, sizeof(endpoint_key), "%u",
                 endpoint.endpoint_id);
        cJSON *endpoint_json = cJSON_AddObjectToObject(endpoints, endpoint_key);
        cJSON *clusters = endpoint_json
                              ? cJSON_AddObjectToObject(endpoint_json, "clusters")
                              : nullptr;
        if (!endpoint_json ||
            !cJSON_AddStringToObject(endpoint_json, "name", endpoint.name) ||
            !clusters) {
            cJSON_Delete(root);
            return nullptr;
        }
        for (size_t cluster_offset = 0;
             cluster_offset < endpoint.cluster_count; ++cluster_offset) {
            const auto &cluster =
                set.clusters[endpoint.first_cluster + cluster_offset];
            cJSON *cluster_json = encode(cluster);
            if (!cluster_json ||
                !cJSON_AddItemToObject(clusters, cluster.adapter->key,
                                       cluster_json)) {
                cJSON_Delete(cluster_json);
                cJSON_Delete(root);
                return nullptr;
            }
        }
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

esp_err_t add_property(cJSON *properties, const char *key, cJSON *value,
                       const char *unit)
{
    json_owner property(cJSON_CreateObject());
    if (!property || !add_owned(property.get(), "value", json_owner(value)) ||
        (unit &&
         !add_owned(property.get(), "unit",
                    json_owner(cJSON_CreateString(unit))))) {
        return ESP_ERR_NO_MEM;
    }
    return add_owned(properties, key, std::move(property)) ? ESP_OK
                                                            : ESP_ERR_NO_MEM;
}

cJSON *add_action(cJSON *actions, const char *key)
{
    json_owner action(cJSON_CreateObject());
    json_owner arguments(cJSON_CreateObject());
    if (!action || !arguments ||
        !add_owned(action.get(), "arguments", std::move(arguments))) {
        return nullptr;
    }
    cJSON *view = cJSON_GetObjectItemCaseSensitive(action.get(), "arguments");
    if (!add_owned(actions, key, std::move(action))) return nullptr;
    return view;
}

esp_err_t add_boolean_argument(cJSON *arguments, const char *key)
{
    return add_owned(arguments, key, argument("boolean")) ? ESP_OK
                                                           : ESP_ERR_NO_MEM;
}

esp_err_t add_integer_argument(cJSON *arguments, const char *key,
                               int32_t minimum, int32_t maximum,
                               const char *unit)
{
    auto value = numeric_argument("integer", minimum, maximum, unit);
    return add_owned(arguments, key, std::move(value)) ? ESP_OK
                                                        : ESP_ERR_NO_MEM;
}

esp_err_t add_number_argument(cJSON *arguments, const char *key,
                              double minimum, double maximum,
                              const char *unit)
{
    auto value = numeric_argument("number", minimum, maximum, unit);
    return add_owned(arguments, key, std::move(value)) ? ESP_OK
                                                        : ESP_ERR_NO_MEM;
}

esp_err_t add_string_argument(cJSON *arguments, const char *key,
                              const char *const *values, size_t value_count)
{
    auto value = argument("string");
    json_owner values_json(cJSON_CreateArray());
    if (!value || !values_json) return ESP_ERR_NO_MEM;
    for (size_t index = 0; index < value_count; ++index) {
        json_owner item(cJSON_CreateString(values[index]));
        if (!item || !cJSON_AddItemToArray(values_json.get(), item.get())) {
            return ESP_ERR_NO_MEM;
        }
        item.release();
    }
    if (!add_owned(value.get(), "enum", std::move(values_json))) {
        return ESP_ERR_NO_MEM;
    }
    return add_owned(arguments, key, std::move(value)) ? ESP_OK
                                                        : ESP_ERR_NO_MEM;
}

const cJSON *only(const cJSON *object, const char *key)
{
    if (!cJSON_IsObject(object) || !object->child || object->child->next) {
        return nullptr;
    }
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return value == object->child ? value : nullptr;
}

bool integer(const cJSON *value, int64_t minimum, int64_t maximum,
             int64_t *out_value)
{
    if (!cJSON_IsNumber(value) || !out_value ||
        !std::isfinite(value->valuedouble) ||
        std::floor(value->valuedouble) != value->valuedouble ||
        value->valuedouble < static_cast<double>(minimum) ||
        value->valuedouble > static_cast<double>(maximum)) {
        return false;
    }
    *out_value = static_cast<int64_t>(value->valuedouble);
    return true;
}

bool number(const cJSON *value, double minimum, double maximum,
            double *out_value)
{
    if (!cJSON_IsNumber(value) || !out_value ||
        !std::isfinite(value->valuedouble) || value->valuedouble < minimum ||
        value->valuedouble > maximum) {
        return false;
    }
    *out_value = value->valuedouble;
    return true;
}

} // namespace data_model::json
