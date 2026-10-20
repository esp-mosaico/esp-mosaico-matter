/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "bridge_dev_mngr.h"
#include "bridge_core_private.h"

#include "esp_matter.h"
#include "esp_matter_bridge.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

// Preserve existing catalogs; NVS namespace names are limited to 15 characters.
#define BRIDGE_DEV_MNGR_NVS_NAMESPACE "mb_dmgr"
#define BRIDGE_DEV_MNGR_NVS_KEY "catalog"
#define BRIDGE_DEV_MNGR_MAX_LISTENERS 4U

namespace bridge {

class lock_guard final {
  public:
    explicit lock_guard(SemaphoreHandle_t mutex) : mutex_(mutex) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    ~lock_guard() { xSemaphoreGive(mutex_); }
    lock_guard(const lock_guard &) = delete;
    lock_guard &operator=(const lock_guard &) = delete;

  private:
    SemaphoreHandle_t mutex_;
};

enum class catalog_state : uint8_t {
    registered = 0,
    removing = 1,
};

struct device_record {
    bridge_dev_mngr_device_definition_t definition{};
    uint16_t endpoint_id = 0;
    catalog_state persistent_state = catalog_state::registered;
    esp_matter_bridge::device_t *bridge = nullptr;
    bool active = false;
    bool reachable = false;
    bool destroyed = false;
    bridge_dev_mngr_profile_t profile{};
    bool has_custom_profile = false;
};

struct listener_entry {
    bridge_dev_mngr_listener_id_t id;
    bridge_dev_mngr_listener_t value;
};

struct cluster_adapter {
    uint32_t cluster_id;
    esp_err_t (*read)(uint16_t endpoint_id,
                      const bridge_dev_mngr_attribute_path_t &path,
                      bridge_dev_mngr_value_t &out_value);
    esp_err_t (*report)(uint16_t endpoint_id,
                        const bridge_dev_mngr_attribute_path_t &path,
                        const bridge_dev_mngr_value_t &value);
};

device_record *find_device(bridge_dev_mngr_handle_t dev_mngr, const char *device_id);
device_record *find_endpoint(bridge_dev_mngr_handle_t dev_mngr, uint16_t endpoint_id);
void copy_info(const device_record &record, bridge_dev_mngr_device_info_t &info);

const cluster_adapter *find_cluster_adapter(uint32_t cluster_id);
esp_err_t generic_attribute_read(uint16_t endpoint_id,
                                 const bridge_dev_mngr_attribute_path_t &path,
                                 bridge_dev_mngr_value_t &out_value);
esp_err_t generic_attribute_report(uint16_t endpoint_id,
                                   const bridge_dev_mngr_attribute_path_t &path,
                                   const bridge_dev_mngr_value_t &value);
esp_err_t attribute_read(device_record &record,
                         const bridge_dev_mngr_attribute_path_t &path,
                         bridge_dev_mngr_value_t &out_value);
esp_err_t attribute_report(device_record &record,
                           const bridge_dev_mngr_attribute_path_t &path,
                           const bridge_dev_mngr_value_t &value);
esp_err_t decode_attribute_value(const esp_matter_attr_val_t &source,
                                 bridge_dev_mngr_value_t &destination);

uint32_t builtin_device_type(const char *profile_id);
esp_err_t build_profile(esp_matter::endpoint_t *endpoint, uint32_t device_type,
                        void *context);

namespace store {
esp_err_t load(bridge_dev_mngr_handle_t dev_mngr);
esp_err_t save(bridge_dev_mngr_handle_t dev_mngr);
esp_err_t erase(bridge_dev_mngr_handle_t dev_mngr);
} // namespace store

void dispatch_device_set_changed(bridge_dev_mngr_handle_t dev_mngr);
void dispatch_attribute_changed(bridge_dev_mngr_handle_t dev_mngr,
                                const bridge_dev_mngr_attribute_change_t &change);

} // namespace bridge

struct bridge_dev_mngr {
    bridge_core_handle_t core;
    nvs_handle_t nvs;
    SemaphoreHandle_t mutex;
    bridge::device_record devices[BRIDGE_DEV_MNGR_MAX_DEVICES];
    bridge::listener_entry listeners[BRIDGE_DEV_MNGR_MAX_LISTENERS];
    bridge_dev_mngr_listener_id_t next_listener_id;
    bridge_dev_mngr_request_id_t next_request_id;
    bool local_attribute_write;
};

esp_err_t bridge_dev_mngr_prepare_start(bridge_dev_mngr_handle_t dev_mngr);
esp_err_t bridge_dev_mngr_factory_reset(bridge_dev_mngr_handle_t dev_mngr);
esp_err_t bridge_dev_mngr_attribute_callback(
    bridge_dev_mngr_handle_t dev_mngr, esp_matter::attribute::callback_type_t phase,
    uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
    esp_matter_attr_val_t *value);
