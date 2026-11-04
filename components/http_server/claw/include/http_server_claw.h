/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "sdkconfig.h"
#include "http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool active;
    bool configured;
    bool completed;
    bool persisted;
    char session_key[64];
    char status[32];
    char message[160];
    char qr_data_url[256];
    char account_id[64];
    char user_id[96];
    char token[256];
    char base_url[160];
} http_server_wechat_login_status_t;

typedef struct {
    esp_err_t (*wechat_login_start)(const char *account_id, bool force);
    esp_err_t (*wechat_login_get_status)(http_server_wechat_login_status_t *status);
    esp_err_t (*wechat_login_cancel)(void);
    esp_err_t (*wechat_login_mark_persisted)(void);
} http_server_claw_services_t;

/** Configure the extension before http_server_init(). NULL selects no login services. */
http_server_extension_t http_server_claw_init(const http_server_claw_services_t *services);
/** Built-in QR login callbacks; empty when the WeChat capability is not compiled.
 * Completed credentials are returned for manual saving through /api/config. */
http_server_claw_services_t http_server_claw_wechat_services(void);
#if CONFIG_APP_CLAW_CAP_IM_LOCAL && CONFIG_HTTPD_WS_SUPPORT
esp_err_t http_server_webim_bind_im(void);
esp_err_t http_server_webim_send_current(const char *message);
#endif

#ifdef __cplusplus
}
#endif
