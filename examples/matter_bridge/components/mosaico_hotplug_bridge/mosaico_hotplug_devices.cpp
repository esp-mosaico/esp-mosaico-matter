/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "mosaico_hotplug_accessory.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {

bridge_dev_mngr_handle_t s_dev_mngr;

struct request_wait {
    SemaphoreHandle_t done;
    std::atomic_int references{2};
    bridge_dev_mngr_request_result_t result{};
};

void release(request_wait *wait)
{
    if (wait->references.fetch_sub(1) == 1) {
        vSemaphoreDelete(wait->done);
        delete wait;
    }
}

void completed(bridge_dev_mngr_handle_t, const bridge_dev_mngr_request_result_t *result, void *ctx)
{
    auto *wait = static_cast<request_wait *>(ctx);
    wait->result = *result;
    xSemaphoreGive(wait->done);
    release(wait);
}

// The caller and callback own separate references: a late completion after a
// timeout never touches the caller's stack or a reused request context.
template <typename Submit>
esp_err_t request(Submit submit, bridge_dev_mngr_device_info_t *out = nullptr,
    bridge_dev_mngr_value_t *value_out = nullptr)
{
    auto *wait = new (std::nothrow) request_wait;
    if (!wait) return ESP_ERR_NO_MEM;
    wait->done = xSemaphoreCreateBinary();
    if (!wait->done) { delete wait; return ESP_ERR_NO_MEM; }
    bridge_dev_mngr_request_id_t id;
    esp_err_t err = submit(&id, completed, wait);
    if (err != ESP_OK) release(wait);
    else if (xSemaphoreTake(wait->done, pdMS_TO_TICKS(5000)) != pdTRUE) err = ESP_ERR_TIMEOUT;
    else {
        err = wait->result.error;
        if (err == ESP_OK && out) *out = wait->result.device;
        if (err == ESP_OK && value_out) *value_out = wait->result.value;
    }
    release(wait);
    return err;
}

} // namespace

void mosaico_hotplug_devices_init(bridge_dev_mngr_handle_t dev_mngr)
{
    s_dev_mngr = dev_mngr;
}

esp_err_t mosaico_hotplug_get_devices(bridge_dev_mngr_device_info_t *devices,
    size_t capacity, size_t *count)
{
    return bridge_dev_mngr_get_devices(s_dev_mngr, devices, capacity, count);
}

esp_err_t mosaico_hotplug_register_device(const bridge_dev_mngr_device_definition_t *definition)
{
    return request([&](auto *id, auto cb, auto ctx) {
        return bridge_dev_mngr_register_device(s_dev_mngr, definition, id, cb, ctx);
    });
}

esp_err_t mosaico_hotplug_activate_device(const char *device_id, bridge_dev_mngr_device_info_t *out)
{
    bridge_dev_mngr_device_info_t info{};
    esp_err_t err = bridge_dev_mngr_get_device(s_dev_mngr, device_id, &info);
    if (err != ESP_OK) return err;
    if (info.active) { if (out) *out = info; return ESP_OK; }
    return request([&](auto *id, auto cb, auto ctx) {
        return bridge_dev_mngr_activate_device(s_dev_mngr, device_id, nullptr, id, cb, ctx);
    }, out);
}

esp_err_t mosaico_hotplug_remove_device(const char *device_id)
{
    return request([&](auto *id, auto cb, auto ctx) {
        return bridge_dev_mngr_remove_device(s_dev_mngr, device_id, id, cb, ctx);
    });
}

esp_err_t mosaico_hotplug_deactivate_device(const char *device_id)
{
    return request([&](auto *id, auto cb, auto ctx) {
        return bridge_dev_mngr_deactivate_device(s_dev_mngr, device_id, id, cb, ctx);
    });
}

esp_err_t mosaico_hotplug_set_reachable(const char *device_id, bool reachable)
{
    bridge_dev_mngr_device_update_t update{};
    update.fields = BRIDGE_DEV_MNGR_UPDATE_REACHABLE;
    update.reachable = reachable;
    return request([&](auto *id, auto cb, auto ctx) {
        return bridge_dev_mngr_update_device(s_dev_mngr, device_id, &update, id, cb, ctx);
    });
}

esp_err_t mosaico_hotplug_set_name(const char *device_id, const char *name)
{
    if (!name || !name[0] || strlen(name) >= BRIDGE_DEV_MNGR_NAME_MAX) return ESP_ERR_INVALID_ARG;
    bridge_dev_mngr_device_update_t update{};
    update.fields = BRIDGE_DEV_MNGR_UPDATE_NAME;
    snprintf(update.name, sizeof(update.name), "%s", name);
    return request([&](auto *id, auto cb, auto ctx) {
        return bridge_dev_mngr_update_device(s_dev_mngr, device_id, &update, id, cb, ctx);
    });
}

esp_err_t mosaico_hotplug_report(const char *device_id,
    const bridge_dev_mngr_attribute_path_t *path, const bridge_dev_mngr_value_t *value)
{
    return request([&](auto *id, auto cb, auto ctx) {
        return bridge_dev_mngr_report_attribute(s_dev_mngr, device_id, path, value, id, cb, ctx);
    });
}

esp_err_t mosaico_hotplug_read(const char *device_id,
    const bridge_dev_mngr_attribute_path_t *path, bridge_dev_mngr_value_t *value)
{
    return request([&](auto *id, auto cb, auto ctx) {
        return bridge_dev_mngr_read_attribute(s_dev_mngr, device_id, path, id, cb, ctx);
    }, nullptr, value);
}
