/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "http_server_priv.h"

bool http_server_matter_enabled(void)
{
    return false;
}

esp_err_t http_server_register_matter_routes(httpd_handle_t server)
{
    (void)server;
    return ESP_OK;
}
