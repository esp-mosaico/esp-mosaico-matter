/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "http_server";

static EXT_RAM_BSS_ATTR http_server_ctx_t s_ctx;

http_server_ctx_t *http_server_ctx(void)
{
    return &s_ctx;
}

esp_err_t http_server_captive_404_handler(httpd_req_t *req, httpd_err_code_t error)
{
    if (strncmp(req->uri, "/api/", 5) == 0 ||
        strncmp(req->uri, "/ws/", 4) == 0 ||
        strncmp(req->uri, "/lua/", 5) == 0) {
        return httpd_resp_send_err(req, error, NULL);
    }
    http_server_wifi_status_t status = {0};
    esp_err_t err = s_ctx.services.get_wifi_status ? s_ctx.services.get_wifi_status(&status) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK || !status.ap_active) {
        return httpd_resp_send_err(req, error, NULL);
    }

    const char *ap_ip = (status.ap_ip && status.ap_ip[0]) ? status.ap_ip : "192.168.4.1";
    char location[48];
    /* The QR code only joins the provisioning AP. Once the phone's captive
     * portal probe reaches us, land directly on System Settings / Basic,
     * where the target STA SSID and password are configured. */
    snprintf(location, sizeof(location), "http://%s/#basic", ap_ip);

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t http_server_init(const http_server_config_t *config)
{
    if (!config || !config->storage_base_path || config->storage_base_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->services.read_config || !config->services.patch_config || !config->services.get_wifi_status) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.server) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    strlcpy(s_ctx.storage_base_path, config->storage_base_path, sizeof(s_ctx.storage_base_path));
    s_ctx.services = config->services;
    s_ctx.extension = config->extension;
    s_ctx.claw_enabled = config->claw_enabled;
    s_ctx.ap_config_supported = config->ap_config_supported;
    s_ctx.asr_supported = config->asr_supported;
    return ESP_OK;
}

static void http_server_close_fn(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    if (s_ctx.extension.close_session) {
        s_ctx.extension.close_session(sockfd);
    }
    close(sockfd);
}

esp_err_t http_server_start(void)
{
    if (!s_ctx.services.read_config) return ESP_ERR_INVALID_STATE;
    if (s_ctx.server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = HTTP_SERVER_CTRL_PORT;
    config.max_uri_handlers = 32;
    config.stack_size = 8192;
    config.max_open_sockets = 12;
    config.lru_purge_enable = true;

    uint32_t task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= config.stack_size) {
        task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }
#endif
    config.task_caps = task_caps;

    config.close_fn = http_server_close_fn;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&s_ctx.server, &config), TAG, "Failed to start HTTP server");
    esp_err_t (*const registrars[])(httpd_handle_t) = {
        http_server_register_assets_routes,
        http_server_register_config_routes,
        http_server_register_status_routes,
        http_server_register_files_routes,
        http_server_register_matter_routes,
        s_ctx.extension.register_routes,
    };
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(registrars) / sizeof(registrars[0]); ++i) {
        if (registrars[i] && (err = registrars[i](s_ctx.server)) != ESP_OK) {
            break;
        }
    }
    if (err == ESP_OK) {
        err = httpd_register_err_handler(s_ctx.server, HTTPD_404_NOT_FOUND,
                                        http_server_captive_404_handler);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP route registration failed: %s", esp_err_to_name(err));
        (void)http_server_stop();
    }
    return err;
}

esp_err_t http_server_stop(void)
{
    if (!s_ctx.server) {
        return ESP_OK;
    }

    if (s_ctx.extension.stop) {
        s_ctx.extension.stop();
    }
    esp_err_t err = httpd_stop(s_ctx.server);
    if (err == ESP_OK) {
        s_ctx.server = NULL;
    }
    return err;
}
