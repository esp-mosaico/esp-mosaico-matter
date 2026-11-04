/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "http_server_priv.h"
#include "http_server_matter_bridge.h"
#include <stdlib.h>

static bridge_core_handle_t s_core;
static bridge_dev_mngr_handle_t s_manager;

esp_err_t http_server_matter_set_bridge(bridge_core_handle_t core,
                                       bridge_dev_mngr_handle_t manager)
{
    if ((core == NULL) != (manager == NULL)) return ESP_ERR_INVALID_ARG;
    if (http_server_ctx()->server) return ESP_ERR_INVALID_STATE;
    s_core = core;
    s_manager = manager;
    return ESP_OK;
}

bool http_server_matter_enabled(void)
{
    return s_core && s_manager;
}

static esp_err_t bridge_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);
    cJSON_AddStringToObject(root, "error", message);
    httpd_resp_set_status(req, status);
    return http_server_send_json_response(req, root);
}

static esp_err_t bridge_info_handler(httpd_req_t *req)
{
    bool commissioned = false;
    if (!http_server_matter_enabled() ||
        bridge_core_is_commissioned(s_core, &commissioned) != ESP_OK) {
        return bridge_error(req, "503 Service Unavailable", "Matter bridge is not ready");
    }
    char qr_code[200] = {0}, manual_code[32] = {0};
    if (!commissioned &&
        (bridge_core_get_qr_code(s_core, qr_code, sizeof(qr_code)) != ESP_OK ||
         bridge_core_get_manual_pairing_code(s_core, manual_code, sizeof(manual_code)) != ESP_OK)) {
        return bridge_error(req, "500 Internal Server Error", "Unable to read pairing codes");
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);
    cJSON_AddBoolToObject(root, "commissioned", commissioned);
    cJSON_AddNumberToObject(root, "max_devices", BRIDGE_DEV_MNGR_MAX_DEVICES);
    http_server_json_add_string(root, "qr_code", qr_code);
    http_server_json_add_string(root, "manual_pairing_code", manual_code);
    return http_server_send_json_response(req, root);
}

static esp_err_t bridge_devices_handler(httpd_req_t *req)
{
    if (!http_server_matter_enabled()) {
        return bridge_error(req, "503 Service Unavailable", "Matter bridge is not ready");
    }
    bridge_dev_mngr_device_info_t *devices = calloc(BRIDGE_DEV_MNGR_MAX_DEVICES, sizeof(*devices));
    if (!devices) return httpd_resp_send_500(req);
    size_t count = 0;
    esp_err_t err = bridge_dev_mngr_get_devices(s_manager, devices, BRIDGE_DEV_MNGR_MAX_DEVICES, &count);
    if (err != ESP_OK) {
        free(devices);
        return bridge_error(req, "503 Service Unavailable", "Unable to read bridge devices");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *items = root ? cJSON_AddArrayToObject(root, "devices") : NULL;
    if (!items) {
        free(devices);
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }
    for (size_t i = 0; i < count; ++i) {
        const bridge_dev_mngr_device_info_t *device = &devices[i];
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddItemToArray(items, item)) {
            cJSON_Delete(item);
            free(devices);
            cJSON_Delete(root);
            return httpd_resp_send_500(req);
        }
        cJSON_AddStringToObject(item, "id", device->definition.id);
        cJSON_AddStringToObject(item, "name", device->definition.name);
        cJSON_AddStringToObject(item, "profile_id", device->definition.profile_id);
        cJSON_AddNumberToObject(item, "device_type", device->definition.device_type);
        cJSON_AddNumberToObject(item, "endpoint_id", device->endpoint_id);
        cJSON_AddBoolToObject(item, "active", device->active);
        cJSON_AddBoolToObject(item, "reachable", device->reachable);
    }
    free(devices);
    return http_server_send_json_response(req, root);
}

static esp_err_t bridge_window_handler(httpd_req_t *req)
{
    bool commissioned = false;
    if (!http_server_matter_enabled() ||
        bridge_core_is_commissioned(s_core, &commissioned) != ESP_OK) {
        return bridge_error(req, "503 Service Unavailable", "Matter bridge is not ready");
    }
    if (commissioned) {
        return bridge_error(req, "409 Conflict", "Bridge is already commissioned");
    }
    http_server_wifi_status_t wifi = {0};
    if (http_server_ctx()->services.get_wifi_status(&wifi) != ESP_OK) {
        return bridge_error(req, "503 Service Unavailable", "Unable to read Wi-Fi status");
    }
    esp_err_t err = wifi.wifi_connected ?
        bridge_core_open_on_network_commissioning_window(s_core) :
        bridge_core_open_commissioning_window(s_core);
    if (err != ESP_OK) {
        return bridge_error(req, err == ESP_ERR_INVALID_STATE ?
            "409 Conflict" : "500 Internal Server Error", "Unable to open commissioning window");
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "duration_seconds", 180);
    cJSON_AddStringToObject(root, "transport", wifi.wifi_connected ? "on_network" : "ble");
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_matter_routes(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/matter/bridge", .method = HTTP_GET, .handler = bridge_info_handler},
        {.uri = "/api/matter/bridge/devices", .method = HTTP_GET, .handler = bridge_devices_handler},
        {.uri = "/api/matter/bridge/commissioning-window", .method = HTTP_POST, .handler = bridge_window_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
