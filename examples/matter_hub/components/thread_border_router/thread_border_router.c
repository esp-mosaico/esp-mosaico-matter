/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "thread_border_router.h"

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_spinel.h"
#include "esp_vfs_eventfd.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "mosaico_module_mgr.h"
#include "nvs.h"
#include "openthread/dataset_ftd.h"
#include "openthread/thread.h"

#define THREAD_BR_NVS "thread_br"
#define RCP_EN GPIO_NUM_46
#define RCP_BOOT GPIO_NUM_47
#define RCP_BOARD_TYPE ((mosaico_board_type_t)0x30)
#define RCP_SLOT MOSAICO_MODULE_MGR_SLOT_RIGHT
static const char *TAG = "thread_br";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static thread_border_router_status_t s_status;
static bool s_present;
static bool s_enabled;
static unsigned s_generation;
static otOperationalDatasetTlvs s_dataset;
static bool s_have_dataset;
/* Owned by the manager for its lifetime, including while Thread is stopped. */
static bool s_control_pins_configured;
static bool s_worker_started;
static bool s_border_router_started;

void thread_border_router_get_status(thread_border_router_status_t *status)
{
    if (!status) { return; }
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    status->board_present = s_present;
    status->enabled = s_enabled;
    portEXIT_CRITICAL(&s_lock);
}

static void publish(thread_border_router_status_t status)
{
    portENTER_CRITICAL(&s_lock);
    status.board_present = s_present;
    status.enabled = s_enabled;
    status.commissioning_ready &= s_present && s_enabled;
    s_status = status;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t thread_border_router_set_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_lock);
    TaskHandle_t task = s_task;
    if (task) {
        s_enabled = enabled;
        ++s_generation; /* An explicit Start also retries an application error. */
        if (!enabled) { s_status.commissioning_ready = false; }
    }
    portEXIT_CRITICAL(&s_lock);
    if (!task) { return ESP_ERR_INVALID_STATE; }
    xTaskNotifyGive(task);
    return ESP_OK;
}

esp_err_t thread_border_router_get_dataset(uint8_t *buffer, size_t capacity,
                                           size_t *length, void *ctx)
{
    (void)ctx;
    if (!buffer || !length) { return ESP_ERR_INVALID_ARG; }
    *length = 0;
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_status.commissioning_ready && s_present && s_enabled &&
            s_have_dataset) {
        if (capacity >= s_dataset.mLength) {
            *length = s_dataset.mLength;
            memcpy(buffer, s_dataset.mTlvs, *length);
            error = ESP_OK;
        } else {
            error = ESP_ERR_INVALID_SIZE;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return error;
}

static void reset_rcp(void)
{
    /* The expansion rail is shared with CameraBoard. Reset only the H2. */
    gpio_set_level(RCP_BOOT, 1);
    gpio_set_level(RCP_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(RCP_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
}

static esp_err_t init_mdns(void)
{
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "Initialize shared mDNS");
    char hostname[MDNS_NAME_BUF_LEN];
    esp_err_t error = mdns_hostname_get(hostname);
    if (error == ESP_ERR_INVALID_STATE) {
        uint8_t mac[6];
        ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, mac), TAG, "Read mDNS MAC");
        snprintf(hostname, sizeof(hostname), "mosaico-br-%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        // Delegated SRP service adds also require a local mDNS hostname.
        // This blocks on the mDNS task, so call it before taking the OT lock.
        ESP_RETURN_ON_ERROR(mdns_hostname_set(hostname), TAG, "Set mDNS hostname");
        error = mdns_hostname_get(hostname);
    }
    ESP_RETURN_ON_ERROR(error, TAG, "Read mDNS hostname");
    ESP_LOGI(TAG, "SRP publishing via mDNS hostname %s", hostname);
    return ESP_OK;
}

static bool is_rcp(const mosaico_module_mgr_info_t *info)
{
    return info->slot == RCP_SLOT &&
        info->presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
        info->descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID &&
        info->eeprom.board_type == RCP_BOARD_TYPE &&
        strncmp(info->eeprom.board_name, "Thread-RCP", sizeof(info->eeprom.board_name)) == 0;
}

/* Only used for a healthy stack, never as a terminal RCP failure handler.
 * A cleanup failure is fatal: do not release a slot with a live OT worker. */
static void stop_network(void)
{
    portENTER_CRITICAL(&s_lock);
    s_status.commissioning_ready = false;
    s_have_dataset = false;
    memset(&s_dataset, 0, sizeof(s_dataset));
    portEXIT_CRITICAL(&s_lock);
    if (!s_worker_started) { return; }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();
    ESP_ERROR_CHECK(instance ? ESP_OK : ESP_ERR_INVALID_STATE);
    ESP_ERROR_CHECK(otThreadSetEnabled(instance, false) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(otIp6SetEnabled(instance, false) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
    if (s_border_router_started) {
        ESP_ERROR_CHECK(esp_openthread_border_router_deinit());
        s_border_router_started = false;
    }
    esp_openthread_lock_release();
    ESP_ERROR_CHECK(esp_openthread_stop());
    s_worker_started = false;
}

static esp_err_t start_network(void)
{
    esp_netif_t *backbone = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!backbone) { return ESP_ERR_INVALID_STATE; }
    ESP_RETURN_ON_ERROR(init_mdns(), TAG, "Prepare SRP service publishing");
    ESP_RETURN_ON_ERROR(gpio_set_level(RCP_BOOT, 1), TAG, "Set boot pin");
    ESP_RETURN_ON_ERROR(gpio_set_level(RCP_EN, 1), TAG, "Set reset pin");
    if (!s_control_pins_configured) {
        const gpio_config_t pins = {
            .pin_bit_mask = BIT64(RCP_EN) | BIT64(RCP_BOOT),
            .mode = GPIO_MODE_OUTPUT,
        };
        /* gpio_config reserves outputs; repeating it reports our own pins
         * as conflicts. Stop keeps EN/BOOT owned and in normal boot mode. */
        ESP_RETURN_ON_ERROR(gpio_config(&pins), TAG, "Configure RCP pins");
        s_control_pins_configured = true;
    }
    esp_openthread_register_rcp_failure_handler(reset_rcp);
    esp_openthread_set_backbone_netif(backbone);
    const esp_openthread_config_t config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config = {
            .radio_config = {
                .radio_mode = RADIO_MODE_UART_RCP,
                .radio_uart_config = {
                    .port = UART_NUM_1,
                    .uart_config = {
                        .baud_rate = 460800,
                        .data_bits = UART_DATA_8_BITS,
                        .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1,
                        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                        .source_clk = UART_SCLK_DEFAULT,
                    },
                    .rx_pin = GPIO_NUM_11,
                    .tx_pin = GPIO_NUM_10,
                },
            },
            .host_config = {.host_connection_mode = HOST_CONNECTION_MODE_NONE},
            .port_config = {
                .storage_partition_name = "nvs",
                .netif_queue_size = 10,
                .task_queue_size = 10,
            },
        },
    };
    esp_err_t error = esp_openthread_start(&config);
    if (error != ESP_OK) { return error; }
    s_worker_started = true;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();
    if (!instance) {
        error = ESP_FAIL;
        goto done;
    }
    otOperationalDatasetTlvs tlvs;
    otError ot_error = otDatasetGetActiveTlvs(instance, &tlvs);
    if (ot_error == OT_ERROR_NOT_FOUND) {
        otOperationalDataset dataset;
        ot_error = otDatasetCreateNewNetwork(instance, &dataset);
        if (ot_error == OT_ERROR_NONE) {
            uint8_t mac[6];
            if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
                error = ESP_FAIL;
                goto done;
            }
            snprintf(dataset.mNetworkName.m8, sizeof(dataset.mNetworkName.m8),
                     "Mosaico-%02X%02X", mac[4], mac[5]);
            ot_error = otDatasetSetActive(instance, &dataset);
        }
        if (ot_error == OT_ERROR_NONE) { ot_error = otDatasetGetActiveTlvs(instance, &tlvs); }
    }
    if (ot_error != OT_ERROR_NONE) {
        error = ESP_FAIL;
        goto done;
    }
    /* Stock BR initialization has no rollback for a partially initialized
     * service. Fail loudly instead of leaving it attached to a deleted worker. */
    ESP_ERROR_CHECK(esp_openthread_border_router_init());
    s_border_router_started = true;
    if (otIp6SetEnabled(instance, true) != OT_ERROR_NONE ||
            otThreadSetEnabled(instance, true) != OT_ERROR_NONE) {
        error = ESP_FAIL;
        goto done;
    }
    portENTER_CRITICAL(&s_lock);
    s_dataset = tlvs;
    s_have_dataset = true;
    portEXIT_CRITICAL(&s_lock);
done:
    esp_openthread_lock_release();
    if (error != ESP_OK) { stop_network(); }
    return error;
}

static esp_err_t save_enabled(bool enabled)
{
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(THREAD_BR_NVS, NVS_READWRITE, &nvs);
    if (error != ESP_OK) { return error; }
    error = nvs_set_u8(nvs, "enabled", enabled);
    if (error == ESP_OK) { error = nvs_commit(nvs); }
    nvs_close(nvs);
    return error;
}

static void manager(void *ctx)
{
    bool persisted = (bool)(uintptr_t)ctx;
    bool running = false;
    bool faulted = false;
    unsigned attempted_generation = 0;
    uint32_t attempted_board_generation = 0;
    bool claimed = false;
    mosaico_module_lease_t lease = {0};
    thread_border_router_status_t status = {0};
    for (;;) {
        mosaico_module_mgr_info_t board = {0};
        esp_err_t board_error = mosaico_module_mgr_get_info(RCP_SLOT, &board);
        bool present = claimed || (board_error == ESP_OK && is_rcp(&board));
        portENTER_CRITICAL(&s_lock);
        s_present = present;
        bool enabled = s_enabled;
        unsigned generation = s_generation;
        portEXIT_CRITICAL(&s_lock);
        esp_err_t preference_error = ESP_OK;
        if (enabled != persisted) {
            preference_error = save_enabled(enabled);
            if (preference_error == ESP_OK) { persisted = enabled; }
        }
        if (generation != attempted_generation || board.generation != attempted_board_generation) {
            faulted = false;
        }
        if (running && !enabled) {
            status.state = THREAD_BR_STOPPING;
            status.commissioning_ready = false;
            publish(status);
            stop_network();
            running = false;
            ESP_ERROR_CHECK(mosaico_module_mgr_release(&lease));
            lease = (mosaico_module_lease_t) {0};
            claimed = false;
            status.last_error = ESP_OK;
        }
        if (preference_error != ESP_OK) {
            status.last_error = preference_error;
            status.state = THREAD_BR_ERROR;
            status.commissioning_ready = false;
            publish(status);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        if (!running && present && enabled && !faulted) {
            attempted_generation = generation;
            status.state = THREAD_BR_STARTING;
            status.last_error = ESP_OK;
            publish(status);
            const mosaico_module_mgr_claim_config_t claim = {
                .expected_type = RCP_BOARD_TYPE,
                .slot = RCP_SLOT,
                .timeout_ms = 0,
                .flags = MOSAICO_MODULE_CLAIM_USE_SLOT_CONTROL_GPIO,
            };
            status.last_error = mosaico_module_mgr_claim(&claim, &lease);
            if (status.last_error == ESP_OK) {
                claimed = true;
                status.last_error = start_network();
            }
            running = status.last_error == ESP_OK;
            faulted = !running;
            if (!running && claimed) {
                ESP_ERROR_CHECK(mosaico_module_mgr_release(&lease));
                lease = (mosaico_module_lease_t) {0};
                claimed = false;
            }
            /* Claim/release also changes generation. Do not mistake our own
             * release for reinsertion and retry a startup error every poll. */
            if (mosaico_module_mgr_get_info(RCP_SLOT, &board) == ESP_OK) {
                attempted_board_generation = board.generation;
            }
            /* Re-read commands and presence after the potentially slow handshake. */
            continue;
        }
        status.commissioning_ready = false;
        status.backbone_connected = false;
        if (running) {
            wifi_ap_record_t ap;
            status.backbone_connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
            esp_openthread_lock_acquire(portMAX_DELAY);
            otInstance *instance = esp_openthread_get_instance();
            ESP_ERROR_CHECK(instance ? ESP_OK : ESP_ERR_INVALID_STATE);
            otDeviceRole role = otThreadGetDeviceRole(instance);
            snprintf(status.role, sizeof(status.role), "%s", otThreadDeviceRoleToString(role));
            snprintf(status.network_name, sizeof(status.network_name), "%s",
                     otThreadGetNetworkName(instance));
            otOperationalDataset dataset;
            otOperationalDatasetTlvs tlvs;
            if (otDatasetGetActive(instance, &dataset) == OT_ERROR_NONE) {
                status.channel = dataset.mChannel;
            }
            if (otDatasetGetActiveTlvs(instance, &tlvs) == OT_ERROR_NONE) {
                portENTER_CRITICAL(&s_lock);
                s_dataset = tlvs;
                portEXIT_CRITICAL(&s_lock);
            }
            status.commissioning_ready = role >= OT_DEVICE_ROLE_CHILD &&
                otIp6IsEnabled(instance) && status.backbone_connected;
            esp_openthread_lock_release();
            status.state = THREAD_BR_RUNNING;
        } else {
            status.state = !present ? THREAD_BR_UNAVAILABLE :
                faulted && enabled ? THREAD_BR_ERROR : THREAD_BR_STOPPED;
            snprintf(status.role, sizeof(status.role), "disabled");
        }
        publish(status);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    }
}

esp_err_t thread_border_router_init(void)
{
    if (s_task) { return ESP_OK; }
    const esp_vfs_eventfd_config_t eventfd_config = {.max_fds = 8};
    esp_err_t error = esp_vfs_eventfd_register(&eventfd_config);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) { return error; }
    uint8_t enabled = 1;
    nvs_handle_t nvs;
    error = nvs_open(THREAD_BR_NVS, NVS_READONLY, &nvs);
    if (error == ESP_OK) {
        error = nvs_get_u8(nvs, "enabled", &enabled);
        nvs_close(nvs);
    }
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) { return error; }
    ESP_RETURN_ON_ERROR(mosaico_module_mgr_init(NULL), TAG, "Initialize EEPROM detection");
    s_enabled = enabled != 0;
    portENTER_CRITICAL(&s_lock);
    s_present = false;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Thread-RCP detection: slot=right expected name=Thread-RCP type=0x30; auto-start=%s",
             s_enabled ? "enabled" : "disabled");
    if (xTaskCreate(manager, "thread_manager", 6144, (void *)(uintptr_t)s_enabled,
                    3, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
