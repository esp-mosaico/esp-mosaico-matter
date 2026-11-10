/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

void http_server_json_add_string(cJSON *root, const char *key, const char *value);
/** Consumes root, including on error. */
esp_err_t http_server_send_json_response(httpd_req_t *req, cJSON *root);
esp_err_t http_server_parse_json_body(httpd_req_t *req, cJSON **out_root);
void http_server_json_read_string(cJSON *root, const char *key, char *buffer, size_t size);
esp_err_t http_server_query_get(httpd_req_t *req, const char *key, char *value, size_t size);
bool http_server_path_is_safe(const char *path);
esp_err_t http_server_resolve_storage_path(const char *path, char *full_path, size_t size);

#ifdef __cplusplus
}
#endif
