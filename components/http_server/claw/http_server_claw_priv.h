/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "sdkconfig.h"
#include "http_server_claw.h"
#include "http_server_helpers.h"

typedef struct {
    http_server_claw_services_t services;
} http_server_claw_ctx_t;

http_server_claw_ctx_t *http_server_claw_ctx(void);
esp_err_t http_server_register_capabilities_routes(httpd_handle_t server);
esp_err_t http_server_register_lua_modules_routes(httpd_handle_t server);
esp_err_t http_server_register_lua_app_routes(httpd_handle_t server);
esp_err_t http_server_register_wechat_routes(httpd_handle_t server);
esp_err_t http_server_register_webim_routes(httpd_handle_t server);
void http_server_webim_ws_fd_remove(int fd);
void http_server_webim_stop(void);
