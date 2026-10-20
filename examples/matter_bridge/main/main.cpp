/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "app_claw.h"
#include "app_config.h"
#include "bridge_memory.h"
#include "claw_paths.h"
#include "esp_board_manager.h"
#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_matter.h"
#include <app/server/Server.h>
#include <platform/ESP32/NetworkCommissioningDriver.h>
#include "http_server.h"
#include "bridge_core.h"
#include "bridge_dev_mngr.h"
#include "claw_matter_bridge.h"
#include "nvs_flash.h"
#include "esp_app_desc.h"
#if CONFIG_APP_CLAW_MOSAIC_GSP_ENABLE
#include "mosaico_hotplug_bridge.h"
#include "mosaico_matter_camera.h"
#include "mosaico_ui.h"
#include "mosaic_matter_bridge.h"
#include "mosaic_settings.h"
#include "mosaic_system.h"
#endif
#if CONFIG_ESP_BOARD_ESP_MOSAICO
#include "mosaico_bsp_bmgr_bridge.h"
#endif

#if defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && \
    !defined(CONFIG_SPI_FLASH_AUTO_SUSPEND) && \
    (!defined(CONFIG_SPIRAM_FETCH_INSTRUCTIONS) || !defined(CONFIG_SPIRAM_RODATA))
#error "Bridge PSRAM task stacks require CONFIG_SPIRAM_XIP_FROM_PSRAM; enable it using menuconfig."
#endif

static const char *TAG = "matter_bridge";
static SemaphoreHandle_t s_config_lock;
static bool s_claw_started;
static char s_ip[48];
static bridge_core_handle_t s_matter_core;
static bridge_dev_mngr_handle_t s_matter_dev_mngr;

/* Keep the Matter Network Commissioning driver's staged network in sync with
 * local CLAW edits. Updating only esp_wifi_set_config() leaves that driver
 * unaware of the new SSID and can restore an old fail-safe backup on reboot. */
static esp_err_t configure_wifi(const char *ssid, const char *password)
{
    if (!ssid || !password) return ESP_ERR_INVALID_ARG;
    const size_t ssid_len = strlen(ssid), password_len = strlen(password);
    if (ssid_len > 32 || password_len > 63 ||
        (password_len && password_len < 8) || (!ssid_len && password_len)) return ESP_ERR_INVALID_ARG;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (!chip::Server::GetInstance().GetFailSafeContext().IsFailSafeFullyDisarmed()) return ESP_ERR_INVALID_STATE;
    auto &driver = chip::DeviceLayer::NetworkCommissioning::ESPWiFiDriver::GetInstance();
    chip::DeviceLayer::NetworkCommissioning::Network old_network{};
    auto *networks = driver.GetNetworks();
    if (!networks) return ESP_ERR_NO_MEM;
    const bool have_network = networks->Next(old_network);
    networks->Release();
    char debug_buffer[128]{};
    chip::MutableCharSpan debug(debug_buffer);
    uint8_t index = 0;
    using chip::DeviceLayer::NetworkCommissioning::Status;
    const chip::ByteSpan new_ssid(reinterpret_cast<const uint8_t *>(ssid), ssid_len);
    if (have_network && (old_network.networkIDLen != ssid_len ||
            memcmp(old_network.networkID, ssid, ssid_len) != 0)) {
        if (driver.RemoveNetwork(chip::ByteSpan(old_network.networkID, old_network.networkIDLen), debug, index) != Status::kSuccess)
            return ESP_FAIL;
    }
    CHIP_ERROR error = CHIP_NO_ERROR;
    if (ssid_len && driver.AddOrUpdateNetwork(new_ssid,
            chip::ByteSpan(reinterpret_cast<const uint8_t *>(password), password_len), debug, index) != Status::kSuccess) {
        error = CHIP_ERROR_INTERNAL;
    } else {
        error = driver.ConnectWiFiNetwork(ssid, static_cast<uint8_t>(ssid_len),
                                         password, static_cast<uint8_t>(password_len));
    }
    if (error == CHIP_NO_ERROR) error = driver.CommitConfiguration();
    if (error != CHIP_NO_ERROR) {
        (void)driver.RevertConfiguration();
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t load_config(app_config_t *out)
{
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    esp_err_t err = app_config_load(out);
    xSemaphoreGive(s_config_lock);
    wifi_config_t wifi{};
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi) == ESP_OK) {
        snprintf(out->wifi_ssid, sizeof(out->wifi_ssid), "%.*s", 32, wifi.sta.ssid);
        snprintf(out->wifi_password, sizeof(out->wifi_password), "%.*s", 64, wifi.sta.password);
    }
    return err;
}
static esp_err_t apply_config(const app_config_t *config)
{
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    esp_err_t err = app_config_save(config);
    xSemaphoreGive(s_config_lock);
    if (err != ESP_OK || !s_claw_started) return err;
    auto *claw = static_cast<app_claw_config_t *>(calloc(1, sizeof(app_claw_config_t)));
    if (!claw) return ESP_ERR_NO_MEM;
    app_config_to_claw(config, claw);
    err = app_claw_apply_config(claw);
    free(claw);
    return err;
}
static esp_err_t save_config(const app_config_t *config)
{
    const char *message = nullptr;
    esp_err_t err = app_config_validate_wifi(config, &message);
    if (err != ESP_OK) return err;
    wifi_config_t current{};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &current), TAG, "Read Wi-Fi configuration");
    bool changed = strncmp(config->wifi_ssid, reinterpret_cast<const char *>(current.sta.ssid), 32) ||
        strncmp(config->wifi_password, reinterpret_cast<const char *>(current.sta.password), 64);
    if (changed) ESP_RETURN_ON_ERROR(configure_wifi(config->wifi_ssid, config->wifi_password), TAG, "Apply Wi-Fi configuration");
    return apply_config(config);
}
static esp_err_t save_claw(const app_claw_config_t *claw, void *)
{
    auto *config = static_cast<app_config_t *>(calloc(1, sizeof(app_config_t)));
    if (!config) return ESP_ERR_NO_MEM;
    esp_err_t err = load_config(config);
    /* Keep app_config's additional Wi-Fi/time fields; CLAW fields share names. */
#define COPY(field) snprintf(config->field, sizeof(config->field), "%s", claw->field)
    COPY(llm_api_key); COPY(llm_backend_type); COPY(llm_model); COPY(llm_base_url);
    COPY(llm_auth_type); COPY(llm_timeout_ms); COPY(llm_max_tokens);
    COPY(llm_default_image_max_bytes); COPY(llm_max_tokens_field); COPY(llm_reasoning_effort);
    COPY(llm_supports_tools); COPY(llm_supports_vision); COPY(llm_image_remote_url_only);
    COPY(qq_app_id); COPY(qq_app_secret); COPY(qq_msg_type); COPY(feishu_app_id); COPY(feishu_app_secret);
    COPY(tg_bot_token); COPY(wechat_token); COPY(wechat_base_url); COPY(wechat_cdn_base_url); COPY(wechat_account_id);
    COPY(enabled_cap_groups); COPY(llm_visible_cap_groups); COPY(enabled_lua_modules);
    COPY(search_brave_key); COPY(search_tavily_key); COPY(search_http_allowlist);
    COPY(asr_provider); COPY(asr_api_key); COPY(asr_workspace_id); COPY(asr_language_hint);
    COPY(asr_model); COPY(asr_endpoint);
#undef COPY
    if (err == ESP_OK) {
        xSemaphoreTake(s_config_lock, portMAX_DELAY);
        err = app_config_save(config);
        xSemaphoreGive(s_config_lock);
    }
    free(config); return err;
}
static bool network_ready(void *)
{
    wifi_ap_record_t record{};
    auto *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info{};
    return esp_wifi_sta_get_ap_info(&record) == ESP_OK && netif &&
        esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr;
}
static esp_err_t wifi_status(http_server_wifi_status_t *out)
{
    *out = {};
    auto *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info{};
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&info.ip));
        out->wifi_connected = network_ready(nullptr);
        out->ip = s_ip;
    }
    out->ap_active = false; out->wifi_mode = "station";
    return ESP_OK;
}
static esp_err_t restart_device(void) { esp_restart(); return ESP_OK; }
#if CONFIG_APP_CLAW_MOSAIC_GSP_ENABLE
/* Wired into the Mosaic Settings app's hold-to-reset control. */
static esp_err_t settings_factory_reset(void *)
{
    esp_err_t err = claw_matter_bridge_factory_reset();
    if (err == ESP_OK) err = app_config_reset_all();
    if (err == ESP_OK) err = bridge_core_factory_reset(s_matter_core);
    if (err != ESP_OK) ESP_LOGE(TAG, "Factory reset cleanup failed: %s", esp_err_to_name(err));
    return err;
}
static mosaic_system_boot_stage_t s_boot_stage = MOSAIC_SYSTEM_BOOT_SETUP;
static bool s_boot_stage_initialized;
static portMUX_TYPE s_wifi_ui_lock = portMUX_INITIALIZER_UNLOCKED;
static uint16_t s_wifi_disconnect_reason;
static uint32_t s_wifi_operation_id;
static bool s_scan_running, s_scan_valid;
static esp_err_t s_scan_error = ESP_ERR_NOT_FINISHED;
static uint32_t s_scan_revision;
static size_t s_scan_count;
static mosaic_settings_wifi_ap_t s_scan_cache[MOSAIC_SETTINGS_WIFI_SCAN_MAX];

/* Saved credentials, rather than the timing of DHCP or Matter commissioning,
 * decide whether boot needs Wi-Fi setup. Factory reset clears those credentials. */
static esp_err_t system_get_boot_stage(void *, mosaic_system_boot_stage_t *stage)
{
    if (!stage) return ESP_ERR_INVALID_ARG;
    if (!s_boot_stage_initialized) {
        wifi_config_t config{};
        ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &config), TAG, "Read boot Wi-Fi configuration");
        s_boot_stage = config.sta.ssid[0] ? MOSAIC_SYSTEM_BOOT_HOME : MOSAIC_SYSTEM_BOOT_SETUP;
        s_boot_stage_initialized = true;
    }
    *stage = s_boot_stage;
    return ESP_OK;
}
static esp_err_t system_set_boot_stage(void *, mosaic_system_boot_stage_t stage)
{
    if (stage < MOSAIC_SYSTEM_BOOT_SETUP || stage > MOSAIC_SYSTEM_BOOT_HOME) return ESP_ERR_INVALID_ARG;
    s_boot_stage = stage;
    s_boot_stage_initialized = true;
    return ESP_OK;
}
/* Read Matter's STA directly on every UI poll. A cached startup snapshot can
 * otherwise keep reporting disconnected after commissioning/reconnection. */
static esp_err_t settings_get_wifi_status(void *, mosaic_settings_network_t *network)
{
    if (!network) return ESP_ERR_INVALID_ARG;
    *network = {};
    wifi_mode_t mode = WIFI_MODE_NULL;
    ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&mode), TAG, "Read Wi-Fi mode");
    network->enabled = mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA;
    network->desired_enabled = network->enabled;
    network->ap_active = mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA;
    network->radio_state = network->enabled ? MOSAIC_SETTINGS_WIFI_RADIO_ON : MOSAIC_SETTINGS_WIFI_RADIO_OFF;
    wifi_config_t config{};
    if (esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK) {
        network->configured = config.sta.ssid[0] != '\0';
        snprintf(network->ssid, sizeof(network->ssid), "%.*s", 32, config.sta.ssid);
    }
    wifi_ap_record_t record{};
    auto *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info{};
    network->connected = network->enabled && esp_wifi_sta_get_ap_info(&record) == ESP_OK &&
        netif && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr;
    if (network->connected) {
        snprintf(network->ip, sizeof(network->ip), IPSTR, IP2STR(&info.ip));
        network->rssi = record.rssi;
    }
    network->state = !network->enabled ? MOSAIC_SETTINGS_WIFI_DISABLED :
        network->connected ? MOSAIC_SETTINGS_WIFI_CONNECTED :
        network->configured ? MOSAIC_SETTINGS_WIFI_CONNECTING : MOSAIC_SETTINGS_WIFI_IDLE;
    network->sta_state = network->connected ? MOSAIC_SETTINGS_WIFI_STA_CONNECTED :
        !network->configured ? MOSAIC_SETTINGS_WIFI_STA_UNCONFIGURED :
        network->enabled ? MOSAIC_SETTINGS_WIFI_STA_CONNECTING : MOSAIC_SETTINGS_WIFI_STA_IDLE;
    portENTER_CRITICAL(&s_wifi_ui_lock);
    network->operation_id = s_wifi_operation_id;
    network->disconnect_reason = s_wifi_disconnect_reason;
    network->scan_revision = s_scan_revision;
    network->scan_error = s_scan_error;
    network->scan_state = s_scan_running ? MOSAIC_SETTINGS_WIFI_SCAN_RUNNING :
        s_scan_valid ? MOSAIC_SETTINGS_WIFI_SCAN_READY :
        s_scan_error == ESP_ERR_NOT_FINISHED ? MOSAIC_SETTINGS_WIFI_SCAN_IDLE : MOSAIC_SETTINGS_WIFI_SCAN_ERROR;
    portEXIT_CRITICAL(&s_wifi_ui_lock);
    if (network->configured && !network->connected) {
        switch (network->disconnect_reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_CONNECTION_FAIL:
            network->state = MOSAIC_SETTINGS_WIFI_AUTH_FAILED;
            network->sta_state = MOSAIC_SETTINGS_WIFI_STA_AUTH_FAILED;
            break;
        case WIFI_REASON_NO_AP_FOUND:
            network->state = MOSAIC_SETTINGS_WIFI_AP_NOT_FOUND;
            network->sta_state = MOSAIC_SETTINGS_WIFI_STA_AP_NOT_FOUND;
            break;
        default:
            break;
        }
    }
    return ESP_OK;
}

static void settings_wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    portENTER_CRITICAL(&s_wifi_ui_lock);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && data) {
        s_wifi_disconnect_reason = static_cast<wifi_event_sta_disconnected_t *>(data)->reason;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_disconnect_reason = 0;
    }
    portEXIT_CRITICAL(&s_wifi_ui_lock);
}

static void settings_wifi_scan_task(void *)
{
    auto *aps = static_cast<wifi_ap_record_t *>(calloc(MOSAIC_SETTINGS_WIFI_SCAN_MAX, sizeof(wifi_ap_record_t)));
    mosaic_settings_wifi_ap_t result[MOSAIC_SETTINGS_WIFI_SCAN_MAX]{};
    uint16_t count = MOSAIC_SETTINGS_WIFI_SCAN_MAX;
    esp_err_t err = aps ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        /* Serialize the blocking scan with Matter commands on this worker,
         * leaving the display task free to render. Matter's scan-done handler
         * must not consume these results before they have been copied. */
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        if (!chip::Server::GetInstance().GetFailSafeContext().IsFailSafeFullyDisarmed()) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            wifi_scan_config_t scan{};
            scan.show_hidden = true;
            err = esp_wifi_scan_start(&scan, true);
            if (err == ESP_OK) err = esp_wifi_scan_get_ap_records(&count, aps);
            if (err == ESP_OK) {
                for (uint16_t i = 0; i < count; ++i) {
                    snprintf(result[i].ssid, sizeof(result[i].ssid), "%.*s", 32, aps[i].ssid);
                    result[i].rssi = aps[i].rssi;
                    result[i].secured = aps[i].authmode != WIFI_AUTH_OPEN;
                }
            }
        }
    }
    free(aps);
    portENTER_CRITICAL(&s_wifi_ui_lock);
    s_scan_running = false;
    s_scan_error = err;
    s_scan_valid = err == ESP_OK;
    s_scan_count = s_scan_valid ? count : 0;
    if (s_scan_valid) memcpy(s_scan_cache, result, s_scan_count * sizeof(*result));
    ++s_scan_revision;
    portEXIT_CRITICAL(&s_wifi_ui_lock);
    vTaskDelete(nullptr);
}

static esp_err_t settings_request_wifi_scan(void *)
{
    portENTER_CRITICAL(&s_wifi_ui_lock);
    const bool running = s_scan_running;
    s_scan_running = true;
    portEXIT_CRITICAL(&s_wifi_ui_lock);
    if (running) return ESP_OK;
    if (xTaskCreate(settings_wifi_scan_task, "bridge_wifi_scan", 4096, nullptr, 4, nullptr) != pdPASS) {
        portENTER_CRITICAL(&s_wifi_ui_lock);
        s_scan_running = false;
        s_scan_valid = false;
        s_scan_count = 0;
        s_scan_error = ESP_ERR_NO_MEM;
        ++s_scan_revision;
        portEXIT_CRITICAL(&s_wifi_ui_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t settings_scan_wifi(void *, mosaic_settings_wifi_ap_t *records, size_t capacity, size_t *out_count)
{
    if (!records || !capacity || !out_count) return ESP_ERR_INVALID_ARG;
    *out_count = 0;
    portENTER_CRITICAL(&s_wifi_ui_lock);
    if (s_scan_running || !s_scan_valid) {
        const esp_err_t err = s_scan_running ? ESP_ERR_NOT_FINISHED : s_scan_error;
        portEXIT_CRITICAL(&s_wifi_ui_lock);
        return err;
    }
    *out_count = capacity < s_scan_count ? capacity : s_scan_count;
    memcpy(records, s_scan_cache, *out_count * sizeof(*records));
    portEXIT_CRITICAL(&s_wifi_ui_lock);
    return ESP_OK;
}

static esp_err_t settings_connect_wifi(void *, const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || !password) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_wifi_ui_lock);
    s_wifi_disconnect_reason = 0;
    ++s_wifi_operation_id;
    portEXIT_CRITICAL(&s_wifi_ui_lock);
    return configure_wifi(ssid, password);
}

static esp_err_t settings_forget_wifi(void *)
{
    return configure_wifi("", "");
}
/* The settings contract also requires a snapshot and display/audio controls.
 * Unsupported display/audio changes return an error. */
static esp_err_t settings_get_snapshot(void *, mosaic_settings_snapshot_t *ret_snapshot)
{
    memset(ret_snapshot, 0, sizeof(*ret_snapshot));
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc) strlcpy(ret_snapshot->software_version, app_desc->version, sizeof(ret_snapshot->software_version));
    ret_snapshot->brightness = 100;
    ret_snapshot->volume = 100;
    ret_snapshot->display_available = true;
    ret_snapshot->audio_available = true;
    (void)settings_get_wifi_status(nullptr, &ret_snapshot->network);
    auto *config = static_cast<app_config_t *>(calloc(1, sizeof(app_config_t)));
    if (!config) return ESP_ERR_NO_MEM;
    if (load_config(config) == ESP_OK) {
        ret_snapshot->im.wechat_configured = config->wechat_token[0] != '\0';
        ret_snapshot->im.qq_configured = config->qq_app_id[0] != '\0';
        ret_snapshot->im.feishu_configured = config->feishu_app_id[0] != '\0';
        ret_snapshot->im.telegram_configured = config->tg_bot_token[0] != '\0';
        strlcpy(ret_snapshot->llm.backend, config->llm_backend_type, sizeof(ret_snapshot->llm.backend));
        strlcpy(ret_snapshot->llm.model, config->llm_model, sizeof(ret_snapshot->llm.model));
        strlcpy(ret_snapshot->llm.base_url, config->llm_base_url, sizeof(ret_snapshot->llm.base_url));
        ret_snapshot->llm.api_key_configured = config->llm_api_key[0] != '\0';
    }
    free(config);
    ret_snapshot->update.state = MOSAIC_SETTINGS_UPDATE_IDLE;
    return ESP_OK;
}
static esp_err_t settings_set_rotation(void *, uint16_t) { return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t settings_set_brightness(void *, int, bool) { return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t settings_set_volume(void *, int, bool) { return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t settings_request_network_reconfigure(void *) { return settings_request_wifi_scan(nullptr); }
#endif
static void mount_filesystems()
{
    esp_vfs_littlefs_conf_t system{};
    system.base_path = "/system"; system.partition_label = "system"; system.read_only = true;
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&system));
    nvs_handle_t boot;
    ESP_ERROR_CHECK(nvs_open("bridge_boot", NVS_READWRITE, &boot));
    uint8_t initialized = 0;
    esp_err_t err = nvs_get_u8(boot, "data_mounted", &initialized);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) ESP_ERROR_CHECK(err);
    esp_vfs_littlefs_conf_t data{};
    data.base_path = "/data"; data.partition_label = "storage";
    data.format_if_mount_failed = !initialized;
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&data));
    ESP_ERROR_CHECK(nvs_set_u8(boot, "data_mounted", 1)); ESP_ERROR_CHECK(nvs_commit(boot)); nvs_close(boot);
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_DATA, "/data"));
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_SYSTEM, "/system"));
}
extern "C" void app_main()
{
    ESP_ERROR_CHECK(bridge_memory_init());
    bridge_memory_log("before storage");
    ESP_ERROR_CHECK(nvs_flash_init()); /* Never silently erase Matter identities on error. */
    mount_filesystems();
    bridge_memory_log("after storage");
    s_config_lock = xSemaphoreCreateMutex();
    if (!s_config_lock) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(app_config_init());
#if CONFIG_ESP_BOARD_ESP_MOSAICO
    ESP_ERROR_CHECK(mosaico_bsp_bmgr_init());
#endif
    ESP_ERROR_CHECK(esp_board_manager_init());
    ESP_ERROR_CHECK(bridge_core_create(&s_matter_core));
    ESP_ERROR_CHECK(bridge_dev_mngr_create(s_matter_core, &s_matter_dev_mngr));
    ESP_ERROR_CHECK(claw_matter_bridge_lua_register_module());
#if CONFIG_APP_CLAW_MOSAIC_GSP_ENABLE
    ESP_ERROR_CHECK(mosaico_matter_camera_init(s_matter_dev_mngr));
    /* Registers each mosaico_hotplug_bridge accessory's Lua module (today:
     * require("mosaico_interact") for the bundled LED driver). Must happen
     * before app_claw_start() brings up the Lua runtime. */
    ESP_ERROR_CHECK(mosaico_hotplug_bridge_register_lua());
#endif
    auto *config = static_cast<app_config_t *>(calloc(1, sizeof(app_config_t)));
    auto *claw = static_cast<app_claw_config_t *>(calloc(1, sizeof(app_claw_config_t)));
    if (!config || !claw) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(app_config_load(config)); app_config_to_claw(config, claw);
    ESP_ERROR_CHECK(app_claw_set_save_config_callback(save_claw, nullptr));
    ESP_ERROR_CHECK(app_claw_set_network_ready_callback(network_ready, nullptr));
    ESP_ERROR_CHECK(app_claw_start(claw)); s_claw_started = true;
    free(claw); free(config);
    bridge_memory_log("after CLAW");
    http_server_config_t http{};
    http.storage_base_path = claw_paths_get(CLAW_PATH_DATA);
    http.services.load_config = load_config; http.services.save_config = save_config;
    http.services.get_wifi_status = wifi_status; http.services.restart_device = restart_device;
    ESP_ERROR_CHECK(http_server_init(&http));
    ESP_ERROR_CHECK(bridge_core_start(s_matter_core));
    const claw_matter_bridge_config_t bridge_config = {
        .core = s_matter_core,
        .dev_mngr = s_matter_dev_mngr,
        .storage_root = "/data/matter_bridge",
        .runner_path = "/system/skills/matter_bridge/scripts/runner.lua",
    };
    ESP_ERROR_CHECK(claw_matter_bridge_start(&bridge_config));
    bridge_memory_log("after Matter");
#if CONFIG_APP_CLAW_MOSAIC_GSP_ENABLE
    /* Hot-plug detection creates/removes devices through the same
     * claw_matter_bridge device-management API as chat/skill commands, so it
     * must start after claw_matter_bridge_start() brings the Matter server
     * and Lua job supervisor up. */
    ESP_ERROR_CHECK(mosaico_hotplug_bridge_start());
    mosaic_settings_ops_t settings_ops{};
    settings_ops.get_snapshot = settings_get_snapshot;
    settings_ops.set_rotation = settings_set_rotation;
    settings_ops.set_brightness = settings_set_brightness;
    settings_ops.set_volume = settings_set_volume;
    settings_ops.get_wifi_status = settings_get_wifi_status;
    settings_ops.request_wifi_scan = settings_request_wifi_scan;
    settings_ops.scan_wifi = settings_scan_wifi;
    settings_ops.connect_wifi = settings_connect_wifi;
    settings_ops.forget_wifi = settings_forget_wifi;
    settings_ops.request_network_reconfigure = settings_request_network_reconfigure;
    settings_ops.factory_reset = settings_factory_reset;
    ESP_ERROR_CHECK(mosaic_settings_configure(&settings_ops));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, settings_wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, settings_wifi_event, nullptr));
    mosaic_system_ops_t system_ops{};
    system_ops.get_boot_stage = system_get_boot_stage;
    system_ops.set_boot_stage = system_set_boot_stage;
    ESP_ERROR_CHECK(mosaic_system_configure(&system_ops));
    mosaic_matter_bridge_set_handles(s_matter_core, s_matter_dev_mngr);
    ESP_ERROR_CHECK(mosaico_ui_start());
#endif
    ESP_ERROR_CHECK(http_server_start()); ESP_ERROR_CHECK(http_server_webim_bind_im());
    bridge_memory_log("ready");
#if CONFIG_APP_CLAW_MOSAIC_GSP_ENABLE
    ESP_LOGI(TAG, "Configure Wi-Fi in CLAW Settings, then open Matter Bridge for on-network pairing");
#else
    ESP_LOGI(TAG, "Commission over BLE, then open the bridge IP address for CLAW settings/chat");
#endif
}
