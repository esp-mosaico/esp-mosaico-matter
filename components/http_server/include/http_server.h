/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool wifi_connected;
    const char *ip;
    bool ap_active;
    const char *ap_ssid;
    const char *ap_ip;
    const char *wifi_mode;
} http_server_wifi_status_t;

typedef struct {
    const char *groups_csv;
    const char *fields_csv;
    bool include_meta;
} http_server_config_query_t;

typedef struct {
    /** Return an owned, filtered and redacted JSON object. The server deletes it. */
    esp_err_t (*read_config)(const http_server_config_query_t *query, cJSON **out);
    /** Patch is borrowed. Return owned JSON on success, or a static error message. */
    esp_err_t (*patch_config)(const cJSON *patch, cJSON **out, const char **message);
    esp_err_t (*get_wifi_status)(http_server_wifi_status_t *status);
    esp_err_t (*restart_device)(void);
} http_server_services_t;

/** Optional routes. register_routes/stop run on the caller of start/stop.
 * stop quiesces producers before HTTP shuts down; close_session only notifies
 * on the HTTP task, and the base server closes the socket afterward. */
typedef struct {
    esp_err_t (*register_routes)(httpd_handle_t server);
    void (*close_session)(int fd);
    void (*stop)(void);
    /** Optional route support, reported by /api/status; defaults to false. */
    bool webim_supported;
    bool wechat_login_supported;
    bool memory_supported;
} http_server_extension_t;

typedef struct {
    const char *storage_base_path;
    http_server_services_t services;
    http_server_extension_t extension;
    bool claw_enabled;
    bool ap_config_supported;
    /** The application consumes ASR settings, even before credentials are set. */
    bool asr_supported;
} http_server_config_t;

esp_err_t http_server_init(const http_server_config_t *config);
esp_err_t http_server_start(void);
esp_err_t http_server_stop(void);

#ifdef __cplusplus
}
#endif
