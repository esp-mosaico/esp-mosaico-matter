/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_matter_private.h"

#include "cJSON.h"
#include "esp_log.h"

#include <cstdio>
#include <inttypes.h>

namespace {

constexpr char kTag[] = "claw_matter_api";

void action_completed(fabric_core_handle_t,
                      const fabric_core_request_result_t *result, void *)
{
    if (!result) {
        ESP_LOGE(kTag, "action completed without a result");
    } else if (result->error != ESP_OK) {
        ESP_LOGW(kTag, "action %" PRIu32 " completed: %s", result->request_id,
                 esp_err_to_name(result->error));
    }
}

} // namespace

esp_err_t claw_matter_list_devices_json(char **out_json)
{
    if (!out_json) return ESP_ERR_INVALID_ARG;
    auto *state = claw_matter::g_controller;
    if (!state) return ESP_ERR_INVALID_STATE;
    return fabric_manager_list_devices_json(state->manager, out_json);
}

esp_err_t claw_matter_get_device_json(fabric_manager_device_id_t device_id,
                                      char **out_json)
{
    if (device_id == 0 || !out_json) return ESP_ERR_INVALID_ARG;
    auto *state = claw_matter::g_controller;
    if (!state) return ESP_ERR_INVALID_STATE;
    return fabric_manager_get_device_json(state->manager, device_id, out_json);
}

esp_err_t claw_matter_get_property_json(
    fabric_manager_device_id_t device_id, uint16_t endpoint_id, const char *cluster,
    const char *property, char **out_json)
{
    if (device_id == 0 || endpoint_id == 0 || !cluster || !cluster[0] ||
        !property || !property[0] || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = nullptr;
    char *detail = nullptr;
    esp_err_t error = claw_matter_get_device_json(device_id, &detail);
    if (error != ESP_OK) return error;

    cJSON *root = cJSON_Parse(detail);
    fabric_manager_free_json(detail);
    char endpoint_key[8] = {};
    snprintf(endpoint_key, sizeof(endpoint_key), "%u", endpoint_id);
    const cJSON *endpoints = cJSON_GetObjectItemCaseSensitive(root, "endpoints");
    const cJSON *endpoint =
        cJSON_GetObjectItemCaseSensitive(endpoints, endpoint_key);
    const cJSON *clusters =
        cJSON_GetObjectItemCaseSensitive(endpoint, "clusters");
    const cJSON *cluster_json =
        cJSON_GetObjectItemCaseSensitive(clusters, cluster);
    const cJSON *properties =
        cJSON_GetObjectItemCaseSensitive(cluster_json, "properties");
    const cJSON *property_json =
        cJSON_GetObjectItemCaseSensitive(properties, property);
    const cJSON *value =
        cJSON_GetObjectItemCaseSensitive(property_json, "value");
    if (value) *out_json = cJSON_PrintUnformatted(value);
    cJSON_Delete(root);
    if (!value) return ESP_ERR_NOT_FOUND;
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t claw_matter_invoke_action(
    fabric_manager_device_id_t device_id, uint16_t endpoint_id, const char *cluster,
    const char *action, const char *arguments_json,
    fabric_core_request_id_t *out_request_id)
{
    if (device_id == 0 || endpoint_id == 0 || !cluster || !cluster[0] ||
        !action || !action[0] || !out_request_id) {
        return ESP_ERR_INVALID_ARG;
    }
    auto *state = claw_matter::g_controller;
    if (!state) return ESP_ERR_INVALID_STATE;
    return fabric_manager_invoke_action(state->manager, device_id, endpoint_id, cluster,
                                 action, arguments_json ? arguments_json : "{}",
                                 out_request_id, action_completed, nullptr);
}
