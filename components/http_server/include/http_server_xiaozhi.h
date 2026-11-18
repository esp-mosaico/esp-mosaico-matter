/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool started;
    bool wifi_connected;
    bool server_connected;
    bool activating;
    char state[24];
    char transport[16];
    char activation_code[16];
    char activation_message[192];
    char last_error[96];
} http_server_xiaozhi_status_t;

typedef esp_err_t (*http_server_xiaozhi_status_cb_t)(
    http_server_xiaozhi_status_t *status);

/** Bind a XiaoZhi status provider, or pass NULL to unbind. */
esp_err_t http_server_xiaozhi_set_status_cb(
    http_server_xiaozhi_status_cb_t cb);

#ifdef __cplusplus
}
#endif
