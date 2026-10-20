/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

static esp_err_t status_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    http_server_wifi_status_t status = {0};
    esp_err_t err = ctx->services.get_wifi_status(&status);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read Wi-Fi status");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "wifi_connected", status.wifi_connected);
    http_server_json_add_string(root, "ip", status.ip);
    http_server_json_add_string(root, "storage_base_path", ctx->storage_base_path);
    cJSON_AddBoolToObject(root, "ap_active", status.ap_active);
    http_server_json_add_string(root, "ap_ssid", status.ap_ssid);
    http_server_json_add_string(root, "ap_ip", status.ap_ip);
    http_server_json_add_string(root, "wifi_mode", status.wifi_mode);
    cJSON_AddBoolToObject(root, "matter_enabled", http_server_matter_enabled());

    /* "*_free" are allocator-managed free bytes (RAM left after the static
     * image and, for PSRAM in this build, the XIP .text/.rodata + .ext_ram.bss
     * carve-outs). "psram_size" is the physical chip size. */
    cJSON *memory = cJSON_AddObjectToObject(root, "memory");
    if (memory) {
        cJSON_AddNumberToObject(memory, "free_heap",
                                (double)esp_get_free_heap_size());
        cJSON_AddNumberToObject(memory, "min_free_heap",
                                (double)esp_get_minimum_free_heap_size());
        cJSON_AddNumberToObject(memory, "largest_free_block",
                                (double)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        cJSON_AddNumberToObject(memory, "heap_internal_free",
                                (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
            cJSON_AddNumberToObject(memory, "heap_psram_free",
                                    (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
#if CONFIG_SPIRAM
        cJSON_AddNumberToObject(memory, "psram_size", (double)esp_psram_get_size());
#endif
    }

    return http_server_send_json_response(req, root);
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    esp_err_t err = ctx->services.restart_device ? ctx->services.restart_device() : ESP_ERR_NOT_SUPPORTED;
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to restart device");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "message", "device restart scheduled");
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_status_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/restart", .method = HTTP_POST, .handler = restart_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
