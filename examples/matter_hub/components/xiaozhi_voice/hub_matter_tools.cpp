/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hub_matter_tools.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mcp_data.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_property.h"
#include "esp_mcp_tool.h"
#include "fabric_manager.h"

#define TAG "xz_matter"

static fabric_manager_handle_t s_manager;

static void action_completed(fabric_core_handle_t, const fabric_core_request_result_t *result, void *)
{
    if (result == nullptr) {
        ESP_LOGE(TAG, "action completed without a result");
    } else if (result->error != ESP_OK) {
        ESP_LOGW(TAG, "action %lu completed: %s", static_cast<unsigned long>(result->request_id),
                 esp_err_to_name(result->error));
    } else {
        ESP_LOGI(TAG, "action %lu completed", static_cast<unsigned long>(result->request_id));
    }
}

static esp_mcp_value_t mcp_text(const char *text)
{
    esp_mcp_value_t value = esp_mcp_value_create_string(text != nullptr ? text : "");
    if (value.type == ESP_MCP_VALUE_TYPE_INVALID) {
        return esp_mcp_value_create_bool(false);
    }
    return value;
}

static esp_mcp_value_t list_devices(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "tools/call matter.list_devices {}");
    if (s_manager == nullptr) {
        return mcp_text("local fabric is not running");
    }
    char *json = nullptr;
    esp_err_t err = fabric_manager_list_devices_json(s_manager, &json);
    if (err != ESP_OK) {
        fabric_manager_free_json(json);
        ESP_LOGW(TAG, "tools/call matter.list_devices failed: %s", esp_err_to_name(err));
        return mcp_text(esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "tools/call matter.list_devices result: %s", json != nullptr ? json : "{}");
    esp_mcp_value_t value = mcp_text(json);
    fabric_manager_free_json(json);
    return value;
}

static esp_mcp_value_t get_device(const esp_mcp_property_list_t *properties)
{
    int device_id = esp_mcp_property_list_get_property_int(properties, "device_id");
    ESP_LOGI(TAG, "tools/call matter.get_device {\"device_id\":%d}", device_id);
    if (s_manager == nullptr) {
        return mcp_text("local fabric is not running");
    }
    char *json = nullptr;
    esp_err_t err = fabric_manager_get_device_json(
        s_manager, static_cast<fabric_manager_device_id_t>(device_id), &json);
    if (err != ESP_OK) {
        fabric_manager_free_json(json);
        ESP_LOGW(TAG, "tools/call matter.get_device failed: %s", esp_err_to_name(err));
        return mcp_text(esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "tools/call matter.get_device result: %s", json != nullptr ? json : "{}");
    esp_mcp_value_t value = mcp_text(json);
    fabric_manager_free_json(json);
    return value;
}

static esp_mcp_value_t invoke_action(const esp_mcp_property_list_t *properties)
{
    int device_id = esp_mcp_property_list_get_property_int(properties, "device_id");
    int endpoint_id = esp_mcp_property_list_get_property_int(properties, "endpoint_id");
    const char *cluster = esp_mcp_property_list_get_property_string(properties, "cluster");
    const char *action = esp_mcp_property_list_get_property_string(properties, "action");
    const char *arguments = esp_mcp_property_list_get_property_object(properties, "arguments");
    if (cluster == nullptr) {
        cluster = "";
    }
    if (action == nullptr) {
        action = "";
    }
    if (arguments == nullptr || arguments[0] == '\0') {
        arguments = "{}";
    }
    ESP_LOGI(TAG,
             "tools/call matter.invoke_action {\"device_id\":%d,\"endpoint_id\":%d,"
             "\"cluster\":\"%s\",\"action\":\"%s\",\"arguments\":%s}",
             device_id, endpoint_id, cluster, action, arguments);
    if (s_manager == nullptr) {
        return mcp_text("local fabric is not running");
    }
    fabric_core_request_id_t request_id = 0;
    esp_err_t err = fabric_manager_invoke_action(
        s_manager, static_cast<fabric_manager_device_id_t>(device_id),
        static_cast<uint16_t>(endpoint_id), cluster, action, arguments, &request_id,
        action_completed, nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tools/call matter.invoke_action failed: %s", esp_err_to_name(err));
        return mcp_text(esp_err_to_name(err));
    }
    char reply[48];
    snprintf(reply, sizeof(reply), "submitted request %lu", static_cast<unsigned long>(request_id));
    ESP_LOGI(TAG, "tools/call matter.invoke_action result: %s", reply);
    return mcp_text(reply);
}

static esp_err_t add_property(esp_mcp_tool_t *tool, esp_mcp_property_t *prop)
{
    if (prop == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_mcp_tool_add_property(tool, prop);
    if (err != ESP_OK) {
        esp_mcp_property_destroy(prop);
    }
    return err;
}

static esp_err_t add_tool(esp_mcp_t *mcp, esp_mcp_tool_t *tool)
{
    if (tool == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_mcp_add_tool(mcp, tool);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add tool failed: %s", esp_err_to_name(err));
        esp_mcp_tool_destroy(tool);
    }
    return err;
}

esp_err_t hub_matter_register_tools(esp_mcp_t *mcp, void *manager)
{
    s_manager = static_cast<fabric_manager_handle_t>(manager);
    if (mcp == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_manager == nullptr) {
        ESP_LOGW(TAG, "Local Fabric is not running; Matter voice tools were not registered");
        return ESP_OK;
    }

    esp_mcp_tool_t *tool = esp_mcp_tool_create(
        "matter.list_devices",
        "List Matter devices commissioned on this hub. Returns JSON {\"devices\":[...]}.",
        list_devices);
    esp_err_t err = add_tool(mcp, tool);
    if (err != ESP_OK) {
        return err;
    }

    tool = esp_mcp_tool_create(
        "matter.get_device",
        "Get one commissioned Matter device, including endpoints, clusters, and actions.",
        get_device);
    if (tool == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    err = add_property(tool, esp_mcp_property_create_with_range("device_id", 0, 2147483647));
    if (err != ESP_OK) {
        esp_mcp_tool_destroy(tool);
        return err;
    }
    err = add_tool(mcp, tool);
    if (err != ESP_OK) {
        return err;
    }

    tool = esp_mcp_tool_create(
        "matter.invoke_action",
        "Invoke one action from matter.get_device for this same device_id. "
        "cluster and action must be keys on that device, not copied from another device. "
        "arguments is a JSON object containing exactly the arguments that action advertises. "
        "Use {} when it has none.",
        invoke_action);
    if (tool == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    err = add_property(tool, esp_mcp_property_create_with_range("device_id", 0, 2147483647));
    if (err == ESP_OK) {
        err = add_property(tool, esp_mcp_property_create_with_range("endpoint_id", 0, 65535));
    }
    if (err == ESP_OK) {
        err = add_property(tool, esp_mcp_property_create("cluster", ESP_MCP_PROPERTY_TYPE_STRING));
    }
    if (err == ESP_OK) {
        err = add_property(tool, esp_mcp_property_create("action", ESP_MCP_PROPERTY_TYPE_STRING));
    }
    if (err == ESP_OK) {
        err = add_property(tool, esp_mcp_property_create("arguments", ESP_MCP_PROPERTY_TYPE_OBJECT));
    }
    if (err != ESP_OK) {
        esp_mcp_tool_destroy(tool);
        return err;
    }
    err = add_tool(mcp, tool);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Matter voice tools registered");
    return ESP_OK;
}
