/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_xiaozhi.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "http_server_priv.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static http_server_xiaozhi_status_cb_t s_status_cb;

esp_err_t http_server_xiaozhi_set_status_cb(
    http_server_xiaozhi_status_cb_t cb)
{
    portENTER_CRITICAL(&s_lock);
    s_status_cb = cb;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

bool http_server_xiaozhi_enabled(void)
{
    bool enabled;
    portENTER_CRITICAL(&s_lock);
    enabled = (s_status_cb != NULL);
    portEXIT_CRITICAL(&s_lock);
    return enabled;
}

static esp_err_t xiaozhi_status_handler(httpd_req_t *req)
{
    http_server_xiaozhi_status_cb_t cb;
    portENTER_CRITICAL(&s_lock);
    cb = s_status_cb;
    portEXIT_CRITICAL(&s_lock);

    if (!cb) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                   "XiaoZhi agent is not enabled");
    }

    http_server_xiaozhi_status_t *xz = calloc(1, sizeof(*xz));
    if (!xz) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = cb(xz);
    if (err != ESP_OK) {
        free(xz);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to read XiaoZhi status");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(xz);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "started", xz->started);
    cJSON_AddBoolToObject(root, "wifi_connected", xz->wifi_connected);
    cJSON_AddBoolToObject(root, "server_connected", xz->server_connected);
    cJSON_AddBoolToObject(root, "activating", xz->activating);
    http_server_json_add_string(root, "state", xz->state);
    http_server_json_add_string(root, "transport", xz->transport);
    http_server_json_add_string(root, "activation_code", xz->activation_code);
    http_server_json_add_string(root, "activation_message", xz->activation_message);
    http_server_json_add_string(root, "last_error", xz->last_error);
    free(xz);

    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_xiaozhi_routes(httpd_handle_t server)
{
    const httpd_uri_t handler = {
        .uri = "/api/xiaozhi/status",
        .method = HTTP_GET,
        .handler = xiaozhi_status_handler,
    };
    return httpd_register_uri_handler(server, &handler);
}
