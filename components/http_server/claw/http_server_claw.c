/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "http_server_claw_priv.h"
#include <string.h>
#include "esp_check.h"

static http_server_claw_ctx_t s_ctx;

http_server_claw_ctx_t *http_server_claw_ctx(void)
{
    return &s_ctx;
}

static bool wechat_login_supported(void)
{
    return s_ctx.services.wechat_login_start && s_ctx.services.wechat_login_get_status &&
           s_ctx.services.wechat_login_cancel;
}

static esp_err_t register_routes(httpd_handle_t server)
{
    ESP_RETURN_ON_ERROR(http_server_register_capabilities_routes(server), "http_claw", "Capabilities");
    ESP_RETURN_ON_ERROR(http_server_register_lua_modules_routes(server), "http_claw", "Lua modules");
#if CONFIG_APP_CLAW_CAP_LUA && CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER
    ESP_RETURN_ON_ERROR(http_server_register_lua_app_routes(server), "http_claw", "Lua routes");
#endif
    if (wechat_login_supported()) {
        ESP_RETURN_ON_ERROR(http_server_register_wechat_routes(server), "http_claw", "WeChat");
    }
#if CONFIG_APP_CLAW_CAP_IM_LOCAL && CONFIG_HTTPD_WS_SUPPORT
    ESP_RETURN_ON_ERROR(http_server_register_webim_routes(server), "http_claw", "WebIM");
#endif
    return ESP_OK;
}

http_server_extension_t http_server_claw_init(const http_server_claw_services_t *services)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    if (services) s_ctx.services = *services;
    http_server_extension_t extension = {
        .register_routes = register_routes,
        .wechat_login_supported = wechat_login_supported(),
    };
#if CONFIG_APP_CLAW_CAP_MEMORY
    extension.memory_supported = true;
#endif
#if CONFIG_APP_CLAW_CAP_IM_LOCAL && CONFIG_HTTPD_WS_SUPPORT
    extension.close_session = http_server_webim_ws_fd_remove;
    extension.stop = http_server_webim_stop;
    extension.webim_supported = true;
#endif
    return extension;
}
