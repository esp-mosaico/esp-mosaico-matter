/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "app_config.h"
#include "http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_err_t (*load)(app_config_t *config);
    esp_err_t (*save)(const app_config_t *config);
    bool ap_config_supported;
    bool asr_supported;
} http_app_config_services_t;

/** Configure once before starting HTTP. Keeps application save semantics. */
void http_app_config_init(const http_app_config_services_t *services);
esp_err_t http_app_config_read(const http_server_config_query_t *query, cJSON **out);
esp_err_t http_app_config_patch(const cJSON *patch, cJSON **out, const char **message);

#ifdef __cplusplus
}
#endif
