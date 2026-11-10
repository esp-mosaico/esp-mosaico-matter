/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "http_server_priv.h"
#include <string.h>

static esp_err_t config_get_handler(httpd_req_t *req)
{
    char groups[192], fields[192], meta[8];
    http_server_config_query_t query = {0};
    const char *keys[] = {"groups", "fields", "meta"};
    char *values[] = {groups, fields, meta};
    const size_t sizes[] = {sizeof(groups), sizeof(fields), sizeof(meta)};
    for (size_t i = 0; i < 3; ++i) {
        esp_err_t err = http_server_query_get(req, keys[i], values[i], sizes[i]);
        if (err == ESP_ERR_NOT_FOUND) continue;
        if (err != ESP_OK) {
            return httpd_resp_send_err(req, err == ESP_ERR_NO_MEM ?
                HTTPD_500_INTERNAL_SERVER_ERROR : HTTPD_400_BAD_REQUEST,
                "Invalid configuration query");
        }
        if (i == 0) query.groups_csv = groups;
        if (i == 1) query.fields_csv = fields;
        if (i == 2) query.include_meta = strcmp(meta, "1") == 0 || strcmp(meta, "true") == 0;
    }
    cJSON *root = NULL;
    esp_err_t err = http_server_ctx()->services.read_config(&query, &root);
    if (err != ESP_OK || !root) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to load config");
    }
    return http_server_send_json_response(req, root);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    cJSON *patch = NULL, *response = NULL;
    esp_err_t err = http_server_parse_json_body(req, &patch);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, err == ESP_ERR_NO_MEM ?
            HTTPD_500_INTERNAL_SERVER_ERROR : HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    const char *message = NULL;
    err = http_server_ctx()->services.patch_config(patch, &response, &message);
    cJSON_Delete(patch);
    if (err != ESP_OK || !response) {
        cJSON_Delete(response);
        return httpd_resp_send_err(req, err == ESP_ERR_INVALID_ARG ?
            HTTPD_400_BAD_REQUEST : HTTPD_500_INTERNAL_SERVER_ERROR,
            message ? message : "Failed to save config");
    }
    return http_server_send_json_response(req, response);
}

esp_err_t http_server_register_config_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler },
        { .uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler },
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
