/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "http_server.h"
#include "http_server_helpers.h"

#define HTTP_SERVER_CTRL_PORT         32769
#define HTTP_SERVER_SCRATCH_SIZE      4096
#define HTTP_SERVER_PATH_MAX          256
#define HTTP_SERVER_UPLOAD_MAX_SIZE   (64 *1024 * 1024)

typedef struct {
    httpd_handle_t server;
    char storage_base_path[HTTP_SERVER_PATH_MAX];
    http_server_services_t services;
    http_server_extension_t extension;
    bool claw_enabled;
    bool ap_config_supported;
    bool asr_supported;
} http_server_ctx_t;

http_server_ctx_t *http_server_ctx(void);

char *http_server_alloc_scratch_buffer(void);
void http_server_url_decode_inplace(char *value);
esp_err_t http_server_send_embedded_file(httpd_req_t *req,
                                         const uint8_t *start,
                                         const uint8_t *end,
                                         const char *content_type);
bool http_server_build_child_relative_path(const char *base_path,
                                           const char *entry_name,
                                           char *out_path,
                                           size_t out_path_size);

esp_err_t http_server_register_assets_routes(httpd_handle_t server);
esp_err_t http_server_register_config_routes(httpd_handle_t server);
esp_err_t http_server_register_status_routes(httpd_handle_t server);
esp_err_t http_server_register_files_routes(httpd_handle_t server);
esp_err_t http_server_register_matter_routes(httpd_handle_t server);
bool http_server_matter_enabled(void);
esp_err_t http_server_captive_404_handler(httpd_req_t *req, httpd_err_code_t error);
