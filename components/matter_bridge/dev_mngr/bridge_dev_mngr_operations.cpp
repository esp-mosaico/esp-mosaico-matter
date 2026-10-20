/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "bridge_dev_mngr_private.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "clusters/bridged_device_basic_information/bridged_device_basic_information.h"
#include "esp_matter_mem.h"

namespace {

struct pending_request {
    bridge_dev_mngr_handle_t dev_mngr;
    bridge_dev_mngr_request_id_t request_id;
    bridge_dev_mngr_operation_t operation;
    char device_id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
    bridge_dev_mngr_device_definition_t definition;
    bridge_dev_mngr_device_update_t update;
    bridge_dev_mngr_profile_t profile;
    bool has_profile;
    bridge_dev_mngr_attribute_path_t path;
    bridge_dev_mngr_value_t value;
    bridge_dev_mngr_request_completion_cb_t completion;
    void *user_ctx;
};

bool valid_text(const char *text, size_t capacity)
{
    return text && text[0] && memchr(text, 0, capacity);
}

bool same_definition(const bridge_dev_mngr_device_definition_t &first,
                     const bridge_dev_mngr_device_definition_t &second)
{
    return !strcmp(first.id, second.id) &&
           !strcmp(first.name, second.name) &&
           !strcmp(first.profile_id, second.profile_id) &&
           first.device_type == second.device_type;
}

bool valid_definition(const bridge_dev_mngr_device_definition_t &definition)
{
    if (!valid_text(definition.id, sizeof(definition.id)) ||
        !valid_text(definition.name, sizeof(definition.name)) ||
        !valid_text(definition.profile_id, sizeof(definition.profile_id)) ||
        !definition.device_type) {
        return false;
    }
    const uint32_t builtin =
        bridge::builtin_device_type(definition.profile_id);
    return !builtin || builtin == definition.device_type;
}

bridge_dev_mngr_request_id_t allocate_request_id(bridge_dev_mngr_handle_t dev_mngr)
{
    bridge::lock_guard lock(dev_mngr->mutex);
    do {
        ++dev_mngr->next_request_id;
    } while (!dev_mngr->next_request_id);
    return dev_mngr->next_request_id;
}

void set_local_write(bridge_dev_mngr_handle_t dev_mngr, bool value)
{
    dev_mngr->local_attribute_write = value;
}

esp_err_t set_name(bridge_dev_mngr_handle_t dev_mngr, bridge::device_record &device,
                   const char *name)
{
    if (!device.active) {
        return ESP_OK;
    }
    set_local_write(dev_mngr, true);
    esp_err_t error =
        bridge::clusters::bridged_device_basic_information::set_name(
            device.endpoint_id, name);
    set_local_write(dev_mngr, false);
    return error;
}

esp_err_t set_reachable(bridge_dev_mngr_handle_t dev_mngr,
                        bridge::device_record &device, bool reachable)
{
    set_local_write(dev_mngr, true);
    esp_err_t error =
        bridge::clusters::bridged_device_basic_information::set_reachable(
            device.endpoint_id, reachable);
    set_local_write(dev_mngr, false);
    return error;
}

esp_err_t execute_register(pending_request &request,
                           bridge::device_record *&result_device)
{
    auto *dev_mngr = request.dev_mngr;
    auto *existing = bridge::find_device(dev_mngr, request.definition.id);
    if (existing) {
        result_device = existing;
        return existing->persistent_state == bridge::catalog_state::registered &&
                       same_definition(existing->definition,
                                       request.definition)
                   ? ESP_OK
                   : ESP_ERR_INVALID_STATE;
    }
    bridge::device_record *empty = nullptr;
    for (auto &device : dev_mngr->devices) {
        if (!device.definition.id[0]) {
            empty = &device;
            break;
        }
    }
    if (!empty) {
        return ESP_ERR_NO_MEM;
    }
    {
        bridge::lock_guard lock(dev_mngr->mutex);
        empty->definition = request.definition;
        empty->persistent_state = bridge::catalog_state::registered;
        esp_err_t error = bridge::store::save(dev_mngr);
        if (error != ESP_OK) {
            *empty = {};
            return error;
        }
    }
    result_device = empty;
    bridge::dispatch_device_set_changed(dev_mngr);
    return ESP_OK;
}

bool valid_runtime_profile(const bridge::device_record &device,
                           const pending_request &request)
{
    const uint32_t builtin =
        bridge::builtin_device_type(device.definition.profile_id);
    if (builtin) {
        return !request.has_profile && builtin == device.definition.device_type;
    }
    return request.has_profile && request.profile.build_endpoint &&
           request.profile.device_type == device.definition.device_type &&
           !strcmp(request.profile.id, device.definition.profile_id);
}

void destroy_failed_activation(bridge_dev_mngr_handle_t dev_mngr,
                               bridge::device_record &device,
                               uint16_t endpoint_id)
{
    if (device.bridge) {
        (void)esp_matter::endpoint::destroy(dev_mngr->core->node,
                                            device.bridge->endpoint);
        esp_matter_mem_free(device.bridge);
        device.bridge = nullptr;
    }
    if (endpoint_id) {
        (void)esp_matter_bridge::erase_bridged_device_info(endpoint_id);
    }
    {
        bridge::lock_guard lock(dev_mngr->mutex);
        device.endpoint_id = 0;
        device.active = false;
        device.reachable = false;
        device.destroyed = false;
        device.profile = {};
        device.has_custom_profile = false;
        (void)bridge::store::save(dev_mngr);
    }
}

esp_err_t execute_activate(pending_request &request,
                           bridge::device_record *&result_device)
{
    auto *dev_mngr = request.dev_mngr;
    auto *device = bridge::find_device(dev_mngr, request.device_id);
    result_device = device;
    if (!device ||
        device->persistent_state != bridge::catalog_state::registered) {
        return ESP_ERR_NOT_FOUND;
    }
    if (device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!valid_runtime_profile(*device, request)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request.has_profile) {
        device->profile = request.profile;
        device->has_custom_profile = true;
    } else {
        device->profile = {};
        device->has_custom_profile = false;
    }
    const uint16_t retained_endpoint = device->endpoint_id;
    if (device->bridge && !device->destroyed) {
        esp_err_t error =
            esp_matter::endpoint::enable(device->bridge->endpoint);
        if (error != ESP_OK) {
            bridge::lock_guard lock(dev_mngr->mutex);
            device->profile = {};
            device->has_custom_profile = false;
            return error;
        }
    } else {
        device->destroyed = false;
        device->bridge = retained_endpoint
                             ? esp_matter_bridge::resume_device(
                                   dev_mngr->core->node, retained_endpoint, device)
                             : esp_matter_bridge::create_device(
                                   dev_mngr->core->node,
                                   dev_mngr->core->aggregator_endpoint_id,
                                   device->definition.device_type, device);
        if (!device->bridge) {
            device->profile = {};
            device->has_custom_profile = false;
            return ESP_FAIL;
        }
        const uint16_t assigned =
            esp_matter::endpoint::get_id(device->bridge->endpoint);
        esp_err_t error =
            esp_matter::endpoint::enable(device->bridge->endpoint);
        if (error != ESP_OK) {
            destroy_failed_activation(dev_mngr, *device, assigned);
            return error;
        }
        if (assigned != retained_endpoint) {
            {
                bridge::lock_guard lock(dev_mngr->mutex);
                device->endpoint_id = assigned;
                error = bridge::store::save(dev_mngr);
            }
            if (error != ESP_OK) {
                destroy_failed_activation(dev_mngr, *device, assigned);
                return error;
            }
        }
    }
    {
        bridge::lock_guard lock(dev_mngr->mutex);
        device->endpoint_id =
            esp_matter::endpoint::get_id(device->bridge->endpoint);
        device->active = true;
        device->reachable = false;
    }

    /* Output endpoints always restart off; physical drivers converge from
     * this desired state after they bind to the listener. */
    constexpr bridge_dev_mngr_attribute_path_t on_off{UINT32_C(0x0006),
                                                UINT32_C(0x0000)};
    if (esp_matter::attribute::get(device->endpoint_id, on_off.cluster_id,
                                   on_off.attribute_id)) {
        bridge_dev_mngr_value_t off{};
        off.type = BRIDGE_DEV_MNGR_VALUE_BOOL;
        set_local_write(dev_mngr, true);
        esp_err_t error = bridge::attribute_report(*device, on_off, off);
        set_local_write(dev_mngr, false);
        if (error != ESP_OK) {
            esp_err_t disabled =
                esp_matter::endpoint::disable(device->bridge->endpoint);
            {
                bridge::lock_guard lock(dev_mngr->mutex);
                device->active = false;
                device->reachable = false;
                device->profile = {};
                device->has_custom_profile = false;
            }
            if (disabled != ESP_OK) {
                return disabled;
            }
            return error;
        }
    }
    bridge::dispatch_device_set_changed(dev_mngr);
    return ESP_OK;
}

esp_err_t execute_deactivate(pending_request &request,
                             bridge::device_record *&result_device)
{
    auto *device = bridge::find_device(request.dev_mngr, request.device_id);
    result_device = device;
    if (!device ||
        device->persistent_state != bridge::catalog_state::registered) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!device->active) {
        return ESP_OK;
    }
    if (!device->bridge || device->destroyed) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = esp_matter::endpoint::disable(device->bridge->endpoint);
    if (error == ESP_OK) {
        bridge::lock_guard lock(request.dev_mngr->mutex);
        device->active = false;
        device->reachable = false;
        device->profile = {};
        device->has_custom_profile = false;
    }
    if (error == ESP_OK) {
        bridge::dispatch_device_set_changed(request.dev_mngr);
    }
    return error;
}

esp_err_t execute_remove(pending_request &request,
                         bridge::device_record *&result_device)
{
    auto *dev_mngr = request.dev_mngr;
    auto *device = bridge::find_device(dev_mngr, request.device_id);
    result_device = device;
    if (!device) {
        return ESP_ERR_NOT_FOUND;
    }
    {
        bridge::lock_guard lock(dev_mngr->mutex);
        device->persistent_state = bridge::catalog_state::removing;
        esp_err_t error = bridge::store::save(dev_mngr);
        if (error != ESP_OK) {
            device->persistent_state = bridge::catalog_state::registered;
            return error;
        }
    }
    if (device->bridge && !device->destroyed) {
        esp_err_t error = esp_matter::endpoint::destroy(
            dev_mngr->core->node, device->bridge->endpoint);
        if (error != ESP_OK) {
            return error;
        }
        device->destroyed = true;
    }
    if (device->endpoint_id) {
        esp_err_t error =
            esp_matter_bridge::erase_bridged_device_info(device->endpoint_id);
        if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
            return error;
        }
    }
    if (device->bridge) {
        esp_matter_mem_free(device->bridge);
        device->bridge = nullptr;
    }
    {
        bridge::lock_guard lock(dev_mngr->mutex);
        bridge::device_record tombstone = *device;
        *device = {};
        esp_err_t error = bridge::store::save(dev_mngr);
        if (error != ESP_OK) {
            *device = tombstone;
            return error;
        }
    }
    result_device = nullptr;
    bridge::dispatch_device_set_changed(dev_mngr);
    return ESP_OK;
}

esp_err_t execute_update(pending_request &request,
                         bridge::device_record *&result_device)
{
    auto *dev_mngr = request.dev_mngr;
    auto *device = bridge::find_device(dev_mngr, request.device_id);
    result_device = device;
    if (!device ||
        device->persistent_state != bridge::catalog_state::registered) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t allowed =
        BRIDGE_DEV_MNGR_UPDATE_NAME | BRIDGE_DEV_MNGR_UPDATE_REACHABLE;
    if (!request.update.fields || (request.update.fields & ~allowed) ||
        ((request.update.fields & BRIDGE_DEV_MNGR_UPDATE_NAME) &&
         !valid_text(request.update.name, sizeof(request.update.name))) ||
        ((request.update.fields & BRIDGE_DEV_MNGR_UPDATE_REACHABLE) &&
         !device->active)) {
        return ESP_ERR_INVALID_ARG;
    }
    char old_name[BRIDGE_DEV_MNGR_NAME_MAX];
    memcpy(old_name, device->definition.name, sizeof(old_name));
    const bool old_reachable = device->reachable;
    bool name_applied = false;
    bool reachable_applied = false;
    esp_err_t error = ESP_OK;
    if ((request.update.fields & BRIDGE_DEV_MNGR_UPDATE_NAME) &&
        strcmp(old_name, request.update.name)) {
        error = set_name(dev_mngr, *device, request.update.name);
        name_applied = error == ESP_OK;
    }
    if (error == ESP_OK &&
        (request.update.fields & BRIDGE_DEV_MNGR_UPDATE_REACHABLE) &&
        old_reachable != request.update.reachable) {
        error = set_reachable(dev_mngr, *device, request.update.reachable);
        reachable_applied = error == ESP_OK;
    }
    if (error == ESP_OK) {
        bridge::lock_guard lock(dev_mngr->mutex);
        if (request.update.fields & BRIDGE_DEV_MNGR_UPDATE_NAME) {
            memcpy(device->definition.name, request.update.name,
                   sizeof(device->definition.name));
        }
        if (request.update.fields & BRIDGE_DEV_MNGR_UPDATE_REACHABLE) {
            device->reachable = request.update.reachable;
        }
        if (request.update.fields & BRIDGE_DEV_MNGR_UPDATE_NAME) {
            error = bridge::store::save(dev_mngr);
            if (error != ESP_OK) {
                memcpy(device->definition.name, old_name,
                       sizeof(device->definition.name));
                device->reachable = old_reachable;
            }
        }
    }
    if (error != ESP_OK) {
        if (reachable_applied) {
            (void)set_reachable(dev_mngr, *device, old_reachable);
        }
        if (name_applied) {
            (void)set_name(dev_mngr, *device, old_name);
        }
        return error;
    }
    bridge::dispatch_device_set_changed(dev_mngr);
    return ESP_OK;
}

esp_err_t execute_read(pending_request &request,
                       bridge::device_record *&result_device,
                       bridge_dev_mngr_value_t &out_value)
{
    auto *device = bridge::find_device(request.dev_mngr, request.device_id);
    result_device = device;
    if (!device) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    return bridge::attribute_read(*device, request.path, out_value);
}

esp_err_t execute_report(pending_request &request,
                         bridge::device_record *&result_device)
{
    auto *device = bridge::find_device(request.dev_mngr, request.device_id);
    result_device = device;
    if (!device) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!device->active || request.value.truncated) {
        return ESP_ERR_INVALID_STATE;
    }
    set_local_write(request.dev_mngr, true);
    esp_err_t error =
        bridge::attribute_report(*device, request.path, request.value);
    set_local_write(request.dev_mngr, false);
    return error;
}

void execute(intptr_t context)
{
    auto *request = reinterpret_cast<pending_request *>(context);
    bridge_dev_mngr_request_result_t result{};
    result.request_id = request->request_id;
    result.operation = request->operation;
    result.path = request->path;
    bridge::device_record *device = nullptr;
    switch (request->operation) {
    case BRIDGE_DEV_MNGR_OPERATION_REGISTER:
        result.error = execute_register(*request, device);
        break;
    case BRIDGE_DEV_MNGR_OPERATION_UPDATE:
        result.error = execute_update(*request, device);
        break;
    case BRIDGE_DEV_MNGR_OPERATION_ACTIVATE:
        result.error = execute_activate(*request, device);
        break;
    case BRIDGE_DEV_MNGR_OPERATION_DEACTIVATE:
        result.error = execute_deactivate(*request, device);
        break;
    case BRIDGE_DEV_MNGR_OPERATION_REMOVE:
        result.error = execute_remove(*request, device);
        break;
    case BRIDGE_DEV_MNGR_OPERATION_READ_ATTRIBUTE:
        result.error = execute_read(*request, device, result.value);
        break;
    case BRIDGE_DEV_MNGR_OPERATION_REPORT_ATTRIBUTE:
        result.error = execute_report(*request, device);
        result.value = request->value;
        break;
    default:
        result.error = ESP_ERR_INVALID_ARG;
        break;
    }
    if (device && device->definition.id[0]) {
        bridge::lock_guard lock(request->dev_mngr->mutex);
        bridge::copy_info(*device, result.device);
    }
    request->completion(request->dev_mngr, &result, request->user_ctx);
    delete request;
}

esp_err_t submit(pending_request *request,
                 bridge_dev_mngr_request_id_t *out_request_id)
{
    if (!request || !out_request_id) {
        delete request;
        return ESP_ERR_INVALID_ARG;
    }
    *out_request_id = 0;
    if (!bridge_core_is_running(request->dev_mngr->core)) {
        delete request;
        return ESP_ERR_INVALID_STATE;
    }
    const bridge_dev_mngr_request_id_t request_id = allocate_request_id(request->dev_mngr);
    request->request_id = request_id;
    // execute() may complete and delete request before ScheduleWork returns.
    CHIP_ERROR error = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        execute, reinterpret_cast<intptr_t>(request));
    if (error != CHIP_NO_ERROR) {
        delete request;
        return ESP_FAIL;
    }
    *out_request_id = request_id;
    return ESP_OK;
}

pending_request *make_request(
    bridge_dev_mngr_handle_t dev_mngr, bridge_dev_mngr_operation_t operation,
    bridge_dev_mngr_request_completion_cb_t completion, void *user_ctx)
{
    if (!dev_mngr || !completion) {
        return nullptr;
    }
    return new (std::nothrow) pending_request{
        .dev_mngr = dev_mngr,
        .operation = operation,
        .completion = completion,
        .user_ctx = user_ctx,
    };
}

} // namespace

esp_err_t bridge_dev_mngr_register_device(
    bridge_dev_mngr_handle_t dev_mngr, const bridge_dev_mngr_device_definition_t *definition,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx)
{
    if (!definition || !valid_definition(*definition)) {
        return ESP_ERR_INVALID_ARG;
    }
    auto *request = make_request(dev_mngr, BRIDGE_DEV_MNGR_OPERATION_REGISTER,
                                 completion_cb, user_ctx);
    if (!request) {
        return dev_mngr && completion_cb ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    request->definition = *definition;
    return submit(request, out_request_id);
}

esp_err_t bridge_dev_mngr_update_device(
    bridge_dev_mngr_handle_t dev_mngr, const char *device_id,
    const bridge_dev_mngr_device_update_t *update,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx)
{
    if (!valid_text(device_id, BRIDGE_DEV_MNGR_DEVICE_ID_MAX) || !update) {
        return ESP_ERR_INVALID_ARG;
    }
    auto *request = make_request(dev_mngr, BRIDGE_DEV_MNGR_OPERATION_UPDATE,
                                 completion_cb, user_ctx);
    if (!request) {
        return dev_mngr && completion_cb ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    snprintf(request->device_id, sizeof(request->device_id), "%s", device_id);
    request->update = *update;
    return submit(request, out_request_id);
}

esp_err_t bridge_dev_mngr_activate_device(
    bridge_dev_mngr_handle_t dev_mngr, const char *device_id,
    const bridge_dev_mngr_profile_t *custom_profile,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx)
{
    if (!valid_text(device_id, BRIDGE_DEV_MNGR_DEVICE_ID_MAX) ||
        (custom_profile &&
         (!valid_text(custom_profile->id, sizeof(custom_profile->id)) ||
          !custom_profile->device_type ||
          !custom_profile->build_endpoint))) {
        return ESP_ERR_INVALID_ARG;
    }
    auto *request = make_request(dev_mngr, BRIDGE_DEV_MNGR_OPERATION_ACTIVATE,
                                 completion_cb, user_ctx);
    if (!request) {
        return dev_mngr && completion_cb ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    snprintf(request->device_id, sizeof(request->device_id), "%s", device_id);
    if (custom_profile) {
        request->profile = *custom_profile;
        request->has_profile = true;
    }
    return submit(request, out_request_id);
}

esp_err_t bridge_dev_mngr_deactivate_device(
    bridge_dev_mngr_handle_t dev_mngr, const char *device_id,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx)
{
    if (!valid_text(device_id, BRIDGE_DEV_MNGR_DEVICE_ID_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    auto *request = make_request(dev_mngr, BRIDGE_DEV_MNGR_OPERATION_DEACTIVATE,
                                 completion_cb, user_ctx);
    if (!request) {
        return dev_mngr && completion_cb ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    snprintf(request->device_id, sizeof(request->device_id), "%s", device_id);
    return submit(request, out_request_id);
}

esp_err_t bridge_dev_mngr_remove_device(
    bridge_dev_mngr_handle_t dev_mngr, const char *device_id,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx)
{
    if (!valid_text(device_id, BRIDGE_DEV_MNGR_DEVICE_ID_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    auto *request = make_request(dev_mngr, BRIDGE_DEV_MNGR_OPERATION_REMOVE,
                                 completion_cb, user_ctx);
    if (!request) {
        return dev_mngr && completion_cb ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    snprintf(request->device_id, sizeof(request->device_id), "%s", device_id);
    return submit(request, out_request_id);
}

esp_err_t bridge_dev_mngr_read_attribute(
    bridge_dev_mngr_handle_t dev_mngr, const char *device_id,
    const bridge_dev_mngr_attribute_path_t *path,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx)
{
    if (!valid_text(device_id, BRIDGE_DEV_MNGR_DEVICE_ID_MAX) || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    auto *request = make_request(dev_mngr, BRIDGE_DEV_MNGR_OPERATION_READ_ATTRIBUTE,
                                 completion_cb, user_ctx);
    if (!request) {
        return dev_mngr && completion_cb ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    snprintf(request->device_id, sizeof(request->device_id), "%s", device_id);
    request->path = *path;
    return submit(request, out_request_id);
}

esp_err_t bridge_dev_mngr_report_attribute(
    bridge_dev_mngr_handle_t dev_mngr, const char *device_id,
    const bridge_dev_mngr_attribute_path_t *path, const bridge_dev_mngr_value_t *value,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx)
{
    if (!valid_text(device_id, BRIDGE_DEV_MNGR_DEVICE_ID_MAX) || !path || !value ||
        value->type > BRIDGE_DEV_MNGR_VALUE_JSON || value->truncated) {
        return ESP_ERR_INVALID_ARG;
    }
    auto *request = make_request(dev_mngr, BRIDGE_DEV_MNGR_OPERATION_REPORT_ATTRIBUTE,
                                 completion_cb, user_ctx);
    if (!request) {
        return dev_mngr && completion_cb ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    snprintf(request->device_id, sizeof(request->device_id), "%s", device_id);
    request->path = *path;
    request->value = *value;
    return submit(request, out_request_id);
}
