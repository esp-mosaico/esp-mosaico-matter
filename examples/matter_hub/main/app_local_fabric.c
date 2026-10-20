/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_local_fabric.h"

#include <string.h>

#include "claw_matter_controller.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#if CONFIG_APP_THREAD_BORDER_ROUTER_ENABLE
#include "thread_border_router.h"
#endif

static const char *TAG = "app_local_fabric";

static fabric_core_handle_t s_core;
static fabric_manager_handle_t s_manager;

static esp_err_t get_wifi_credentials(char *ssid, size_t ssid_size,
                                      char *password, size_t password_size,
                                      void *user_ctx)
{
    (void)user_ctx;

    wifi_config_t config = {0};
    esp_err_t error = esp_wifi_get_config(WIFI_IF_STA, &config);
    if (error != ESP_OK) {
        return error;
    }

    const size_t ssid_len =
        strnlen((const char *)config.sta.ssid, sizeof(config.sta.ssid));
    const size_t password_len =
        strnlen((const char *)config.sta.password, sizeof(config.sta.password));
    if (ssid_len == 0 || ssid_len >= ssid_size ||
        password_len >= password_size) {
        memset(&config, 0, sizeof(config));
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(ssid, config.sta.ssid, ssid_len);
    ssid[ssid_len] = '\0';
    memcpy(password, config.sta.password, password_len);
    password[password_len] = '\0';
    memset(&config, 0, sizeof(config));
    return ESP_OK;
}

#if CONFIG_APP_THREAD_BORDER_ROUTER_ENABLE
static esp_err_t get_optional_thread_dataset(uint8_t *buffer, size_t capacity,
                                             size_t *length, void *user_ctx)
{
    esp_err_t error = thread_border_router_get_dataset(
        buffer, capacity, length, user_ctx);
    return error == ESP_ERR_INVALID_STATE ? ESP_ERR_NOT_FOUND : error;
}
#endif

static void delete_local_fabric(void)
{
    if (s_manager != NULL) {
        fabric_manager_delete(s_manager);
        s_manager = NULL;
    }
    if (s_core != NULL) {
        fabric_core_delete(s_core);
        s_core = NULL;
    }
}

esp_err_t app_local_fabric_start(void)
{
    if (s_core != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const fabric_core_config_t config = {
        .max_nodes = 16,
        .subscription_interval_s_min = 0,
        .subscription_interval_s_max = 600,
        .get_wifi_credentials = get_wifi_credentials,
#if CONFIG_APP_THREAD_BORDER_ROUTER_ENABLE
        .get_thread_dataset = get_optional_thread_dataset,
#endif
        .skip_paa_and_cd_checks = false,
    };

    esp_err_t error = fabric_core_create(&config, &s_core);
    if (error != ESP_OK) {
        return error;
    }
    error = fabric_manager_create(s_core, &s_manager);
    if (error != ESP_OK) {
        delete_local_fabric();
        return error;
    }
    error = fabric_core_start(s_core);
    if (error != ESP_OK) {
        delete_local_fabric();
        return error;
    }

    error = claw_matter_controller_start(s_manager);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "CLAW Matter integration unavailable: %s",
                 esp_err_to_name(error));
    }

    ESP_LOGI(TAG, "Local Fabric started");
    return ESP_OK;
}

fabric_core_handle_t app_local_fabric_core(void)
{
    return s_core;
}

fabric_manager_handle_t app_local_fabric_manager(void)
{
    return s_manager;
}
