/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "bridge_dev_mngr_private.h"

#include <cstring>
#include <new>

#include "clusters/bridged_device_basic_information/bridged_device_basic_information.h"

namespace {

bool valid_text(const char *text, size_t capacity)
{
    return text && text[0] && memchr(text, 0, capacity);
}

bridge_dev_mngr_listener_id_t next_listener_id(bridge_dev_mngr_handle_t dev_mngr)
{
    do {
        ++dev_mngr->next_listener_id;
    } while (!dev_mngr->next_listener_id);
    return dev_mngr->next_listener_id;
}

} // namespace

bridge::device_record *bridge::find_device(bridge_dev_mngr_handle_t dev_mngr,
                                     const char *device_id)
{
    if (!dev_mngr || !device_id) {
        return nullptr;
    }
    for (auto &device : dev_mngr->devices) {
        if (device.definition.id[0] &&
            !strcmp(device.definition.id, device_id)) {
            return &device;
        }
    }
    return nullptr;
}

bridge::device_record *bridge::find_endpoint(bridge_dev_mngr_handle_t dev_mngr,
                                       uint16_t endpoint_id)
{
    if (!dev_mngr || !endpoint_id) {
        return nullptr;
    }
    for (auto &device : dev_mngr->devices) {
        if (device.definition.id[0] && device.endpoint_id == endpoint_id) {
            return &device;
        }
    }
    return nullptr;
}

void bridge::copy_info(const device_record &record,
                    bridge_dev_mngr_device_info_t &info)
{
    memset(&info, 0, sizeof(info));
    info.definition = record.definition;
    info.endpoint_id = record.endpoint_id;
    info.active = record.active;
    info.reachable = record.reachable;
}

void bridge::dispatch_device_set_changed(bridge_dev_mngr_handle_t dev_mngr)
{
    bridge_dev_mngr_listener_t listeners[BRIDGE_DEV_MNGR_MAX_LISTENERS]{};
    {
        lock_guard lock(dev_mngr->mutex);
        for (size_t index = 0; index < BRIDGE_DEV_MNGR_MAX_LISTENERS; ++index) {
            if (dev_mngr->listeners[index].id) {
                listeners[index] = dev_mngr->listeners[index].value;
            }
        }
    }
    for (const auto &listener : listeners) {
        if (listener.device_set_changed) {
            listener.device_set_changed(dev_mngr, listener.user_ctx);
        }
    }
}

void bridge::dispatch_attribute_changed(
    bridge_dev_mngr_handle_t dev_mngr, const bridge_dev_mngr_attribute_change_t &change)
{
    bridge_dev_mngr_listener_t listeners[BRIDGE_DEV_MNGR_MAX_LISTENERS]{};
    {
        lock_guard lock(dev_mngr->mutex);
        for (size_t index = 0; index < BRIDGE_DEV_MNGR_MAX_LISTENERS; ++index) {
            if (dev_mngr->listeners[index].id) {
                listeners[index] = dev_mngr->listeners[index].value;
            }
        }
    }
    for (const auto &listener : listeners) {
        if (listener.attribute_changed) {
            listener.attribute_changed(dev_mngr, &change, listener.user_ctx);
        }
    }
}

esp_err_t bridge_dev_mngr_create(bridge_core_handle_t core,
                          bridge_dev_mngr_handle_t *out_dev_mngr)
{
    if (!core || !out_dev_mngr || bridge_core_is_running(core) || core->dev_mngr) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_dev_mngr = nullptr;
    auto *dev_mngr = new (std::nothrow) bridge_dev_mngr{};
    if (!dev_mngr) {
        return ESP_ERR_NO_MEM;
    }
    dev_mngr->core = core;
    dev_mngr->mutex = xSemaphoreCreateMutex();
    if (!dev_mngr->mutex) {
        delete dev_mngr;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = nvs_open(BRIDGE_DEV_MNGR_NVS_NAMESPACE, NVS_READWRITE,
                               &dev_mngr->nvs);
    if (error != ESP_OK) {
        vSemaphoreDelete(dev_mngr->mutex);
        delete dev_mngr;
        return error;
    }
    error = bridge::store::load(dev_mngr);
    if (error == ESP_OK) {
        error = esp_matter_bridge::initialize(core->node, bridge::build_profile);
    }
    if (error == ESP_OK) {
        error = bridge_core_attach_dev_mngr(core, dev_mngr);
    }
    if (error != ESP_OK) {
        nvs_close(dev_mngr->nvs);
        vSemaphoreDelete(dev_mngr->mutex);
        delete dev_mngr;
        return error;
    }
    *out_dev_mngr = dev_mngr;
    return ESP_OK;
}

esp_err_t bridge_dev_mngr_delete(bridge_dev_mngr_handle_t dev_mngr)
{
    if (!dev_mngr || bridge_core_is_running(dev_mngr->core)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = bridge_core_detach_dev_mngr(dev_mngr->core, dev_mngr);
    if (error != ESP_OK) {
        return error;
    }
    nvs_close(dev_mngr->nvs);
    vSemaphoreDelete(dev_mngr->mutex);
    delete dev_mngr;
    return ESP_OK;
}

esp_err_t bridge_dev_mngr_add_listener(bridge_dev_mngr_handle_t dev_mngr,
                                const bridge_dev_mngr_listener_t *listener,
                                bridge_dev_mngr_listener_id_t *out_listener_id)
{
    if (!dev_mngr || !listener || !out_listener_id ||
        (!listener->device_set_changed && !listener->attribute_changed)) {
        return ESP_ERR_INVALID_ARG;
    }
    bridge::lock_guard lock(dev_mngr->mutex);
    for (auto &entry : dev_mngr->listeners) {
        if (!entry.id) {
            entry.id = next_listener_id(dev_mngr);
            entry.value = *listener;
            *out_listener_id = entry.id;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t bridge_dev_mngr_remove_listener(bridge_dev_mngr_handle_t dev_mngr,
                                   bridge_dev_mngr_listener_id_t listener_id)
{
    if (!dev_mngr || !listener_id) {
        return ESP_ERR_INVALID_ARG;
    }
    bridge::lock_guard lock(dev_mngr->mutex);
    for (auto &entry : dev_mngr->listeners) {
        if (entry.id == listener_id) {
            entry = {};
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t bridge_dev_mngr_get_device(bridge_dev_mngr_handle_t dev_mngr, const char *device_id,
                              bridge_dev_mngr_device_info_t *out_device)
{
    if (!dev_mngr || !valid_text(device_id, BRIDGE_DEV_MNGR_DEVICE_ID_MAX) ||
        !out_device) {
        return ESP_ERR_INVALID_ARG;
    }
    bridge::lock_guard lock(dev_mngr->mutex);
    auto *device = bridge::find_device(dev_mngr, device_id);
    if (!device || device->persistent_state == bridge::catalog_state::removing) {
        return ESP_ERR_NOT_FOUND;
    }
    bridge::copy_info(*device, *out_device);
    return ESP_OK;
}

esp_err_t bridge_dev_mngr_get_devices(bridge_dev_mngr_handle_t dev_mngr,
                               bridge_dev_mngr_device_info_t *devices,
                               size_t capacity, size_t *out_count)
{
    if (!dev_mngr || !out_count || (!devices && capacity)) {
        return ESP_ERR_INVALID_ARG;
    }
    bridge::lock_guard lock(dev_mngr->mutex);
    size_t count = 0;
    for (const auto &device : dev_mngr->devices) {
        if (!device.definition.id[0] ||
            device.persistent_state == bridge::catalog_state::removing) {
            continue;
        }
        if (devices && count < capacity) {
            bridge::copy_info(device, devices[count]);
        }
        ++count;
    }
    *out_count = count;
    return devices && capacity < count ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t bridge_dev_mngr_prepare_start(bridge_dev_mngr_handle_t dev_mngr)
{
    if (!dev_mngr || bridge_core_is_running(dev_mngr->core)) {
        return ESP_ERR_INVALID_STATE;
    }
    for (auto &device : dev_mngr->devices) {
        if (!device.definition.id[0] ||
            device.persistent_state != bridge::catalog_state::removing) {
            continue;
        }
        if (device.endpoint_id) {
            esp_err_t error =
                esp_matter_bridge::erase_bridged_device_info(device.endpoint_id);
            if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
                return error;
            }
        }
        {
            bridge::lock_guard lock(dev_mngr->mutex);
            const bridge::device_record tombstone = device;
            device = {};
            esp_err_t error = bridge::store::save(dev_mngr);
            if (error != ESP_OK) {
                device = tombstone;
                return error;
            }
        }
    }
    uint16_t sdk_ids[MAX_BRIDGED_DEVICE_COUNT];
    esp_err_t error = esp_matter_bridge::get_bridged_endpoint_ids(sdk_ids);
    if (error != ESP_OK) {
        return error;
    }
    for (uint16_t endpoint_id : sdk_ids) {
        if (endpoint_id == chip::kInvalidEndpointId) {
            continue;
        }
        bool retained = false;
        for (const auto &device : dev_mngr->devices) {
            retained |= device.definition.id[0] &&
                        device.persistent_state ==
                            bridge::catalog_state::registered &&
                        device.endpoint_id == endpoint_id;
        }
        if (!retained) {
            error = esp_matter_bridge::erase_bridged_device_info(endpoint_id);
            if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
                return error;
            }
        }
    }
    return ESP_OK;
}

esp_err_t bridge_dev_mngr_factory_reset(bridge_dev_mngr_handle_t dev_mngr)
{
    if (!dev_mngr) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = bridge::store::erase(dev_mngr);
    if (error == ESP_OK) {
        bridge::lock_guard lock(dev_mngr->mutex);
        for (auto &device : dev_mngr->devices) {
            device = {};
        }
    }
    return error;
}

esp_err_t bridge_dev_mngr_attribute_callback(
    bridge_dev_mngr_handle_t dev_mngr, esp_matter::attribute::callback_type_t phase,
    uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
    esp_matter_attr_val_t *value)
{
    if (!dev_mngr || dev_mngr->local_attribute_write) {
        return ESP_OK;
    }
    bridge::device_record *device = nullptr;
    bool reachable = false;
    char device_id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX]{};
    {
        bridge::lock_guard lock(dev_mngr->mutex);
        device = bridge::find_endpoint(dev_mngr, endpoint_id);
        if (!device || !device->active) {
            return ESP_OK;
        }
        reachable = device->reachable;
        memcpy(device_id, device->definition.id, sizeof(device_id));
    }
    if (phase == esp_matter::attribute::PRE_UPDATE && !reachable &&
        cluster_id !=
            bridge::clusters::bridged_device_basic_information::kClusterId) {
        return ESP_ERR_INVALID_STATE;
    }
    if (phase != esp_matter::attribute::POST_UPDATE || !value) {
        return ESP_OK;
    }
    bridge_dev_mngr_attribute_change_t change{};
    memcpy(change.device_id, device_id, sizeof(change.device_id));
    change.endpoint_id = endpoint_id;
    change.path = {cluster_id, attribute_id};
    esp_err_t error = bridge::decode_attribute_value(*value, change.value);
    if (error == ESP_ERR_NOT_SUPPORTED) {
        return ESP_OK;
    }
    if (error == ESP_OK) {
        bridge::dispatch_attribute_changed(dev_mngr, change);
    }
    return error;
}
