/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_manager_private.h"

#include <cassert>
#include <cstdio>

#include "esp_check.h"
#include "esp_log.h"

#include <cstdlib>
#include <cstring>

namespace
{

const char *const kTag = "fabric_manager";

data_model::listener *find_listener(fabric_manager_handle_t manager,
                              fabric_manager_listener_id_t id)
{
    for (auto &entry : manager->listeners) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

class listener_call_guard final
{
  public:
    listener_call_guard(fabric_manager_handle_t manager, data_model::listener *entry,
                         data_model::listener_call &call)
        : manager_(manager), entry_(entry), call_(call) {}

    ~listener_call_guard()
    {
        data_model::lock_guard lock(manager_->listener_mutex);
        auto **frame = &manager_->listener_calls;
        while (*frame != &call_) {
            assert(*frame);
            frame = &(*frame)->next;
        }
        *frame = call_.next;
        assert(entry_->active_calls > 0);
        --entry_->active_calls;
        if (entry_->removing && entry_->active_calls == 0) {
            if (entry_->drained) {
                const BaseType_t given = xSemaphoreGive(entry_->drained);
                assert(given == pdTRUE);
                (void)given;
            } else {
                *entry_ = {};
            }
        }
    }

    listener_call_guard(const listener_call_guard &) = delete;
    listener_call_guard &operator=(const listener_call_guard &) = delete;

  private:
    fabric_manager_handle_t manager_;
    data_model::listener *entry_;
    data_model::listener_call &call_;
};

template <typename Callback, typename... Arguments>
void dispatch_listeners(fabric_manager_handle_t manager,
                         Callback fabric_manager_listener_t::*member,
                         Arguments... arguments)
{
    fabric_manager_listener_id_t snapshot[CONFIG_FABRIC_MANAGER_MAX_LISTENERS] = {};
    size_t count = 0;
    {
        data_model::lock_guard lock(manager->listener_mutex);
        for (const auto &entry : manager->listeners) {
            if (entry.id && !entry.removing && entry.value.*member)
                snapshot[count++] = entry.id;
        }
    }

    for (size_t index = 0; index < count; ++index) {
        fabric_manager_listener_t value = {};
        data_model::listener *entry;
        data_model::listener_call call = {};
        call.task = xTaskGetCurrentTaskHandle();
        call.listener_id = snapshot[index];
        {
            data_model::lock_guard lock(manager->listener_mutex);
            entry = find_listener(manager, call.listener_id);
            if (!entry || entry->removing)
                continue;
            value = entry->value;
            ++entry->active_calls;
            call.next = manager->listener_calls;
            // listener_call_guard unlinks this frame before its scope ends.
            // cppcheck-suppress autoVariables
            manager->listener_calls = &call;
        }

        listener_call_guard guard(manager, entry, call);
        // Application code runs without either Manager mutex held.
        (value.*member)(manager, arguments..., value.user_ctx);
    }
}

data_model::device *find_device(data_model::device_set &set, fabric_manager_device_id_t device_id)
{
    for (size_t index = 0; index < set.device_count; ++index) {
        if (set.devices[index].id == device_id)
            return &set.devices[index];
    }
    return nullptr;
}

const data_model::cluster_adapter *find_action_cluster(
    data_model::device_set &set, fabric_manager_device_id_t device_id,
    uint16_t endpoint_id, const char *cluster_key, uint64_t *out_node_id,
    data_model::cluster_state *out_state)
{
    const auto *device = find_device(set, device_id);
    if (!device)
        return nullptr;
    *out_node_id = device->node_id;
    for (size_t endpoint_offset = 0;
         endpoint_offset < device->endpoint_count; ++endpoint_offset) {
        const auto &endpoint =
            set.endpoints[device->first_endpoint + endpoint_offset];
        if (endpoint.endpoint_id != endpoint_id)
            continue;
        for (size_t cluster_offset = 0;
             cluster_offset < endpoint.cluster_count; ++cluster_offset) {
            const auto &cluster =
                set.clusters[endpoint.first_cluster + cluster_offset];
            if (!strcmp(cluster.adapter->key, cluster_key)) {
                *out_state = cluster.state;
                return cluster.adapter;
            }
        }
        return nullptr;
    }
    return nullptr;
}

void on_node_set_changed(fabric_core_handle_t, void *user_ctx)
{
    auto manager = static_cast<fabric_manager_handle_t>(user_ctx);
    uint64_t *node_ids = nullptr;
    size_t node_count = 0;
    esp_err_t error = fabric_core_get_node_ids(manager->core, nullptr, 0, &node_count);
    if (error != ESP_OK && error != ESP_ERR_INVALID_SIZE)
        return;
    if (node_count != 0) {
        node_ids =
            static_cast<uint64_t *>(calloc(node_count, sizeof(uint64_t)));
        if (!node_ids)
            return;
        error =
            fabric_core_get_node_ids(manager->core, node_ids, node_count, &node_count);
        if (error != ESP_OK) {
            free(node_ids);
            return;
        }
    }

    data_model::device_set filtered = {};
    bool changed = false;
    {
        data_model::lock_guard lock(manager->state_mutex);
        error =
            data_model::model::retain_nodes(manager, node_ids, node_count, &filtered);
        changed =
            error == ESP_OK && filtered.device_count != manager->set.device_count;
        if (changed) {
            error =
                data_model::store::save(manager, filtered.devices, filtered.device_count,
                                  manager->next_device_id);
        }
        if (error == ESP_OK && changed) {
            data_model::device_set previous = manager->set;
            manager->set = filtered;
            filtered = {};
            data_model::model::free(&previous);
        }
    }
    data_model::model::free(&filtered);
    free(node_ids);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "failed to reconcile nodes: %s", esp_err_to_name(error));
    } else if (changed) {
        data_model::dispatch_device_set_changed(manager);
    }
}

void on_subscription_changed(fabric_core_handle_t,
                             const fabric_core_subscription_update_t *update,
                             void *user_ctx)
{
    auto manager = static_cast<fabric_manager_handle_t>(user_ctx);
    if (!manager || !update)
        return;
    const bool online = update->state == FABRIC_CORE_SUBSCRIPTION_ACTIVE;
    bool changed = false;
    {
        data_model::lock_guard lock(manager->state_mutex);
        for (size_t index = 0; index < manager->set.device_count; ++index) {
            auto &device = manager->set.devices[index];
            if (device.node_id == update->node_id && device.online != online) {
                device.online = online;
                changed = true;
            }
        }
    }
    if (changed)
        data_model::dispatch_device_set_changed(manager);
}

} // namespace

void data_model::dispatch_device_set_changed(fabric_manager_handle_t manager)
{
    dispatch_listeners(manager, &fabric_manager_listener_t::device_set_changed);
}

void data_model::dispatch_property_changes(fabric_manager_handle_t manager,
                                     data_model::property_change *changes,
                                     size_t change_count)
{
    if (!change_count)
        return;
    auto *output = static_cast<fabric_manager_property_change_t *>(
        calloc(change_count, sizeof(fabric_manager_property_change_t)));
    if (!output)
        return;

    bool complete = true;
    for (size_t index = 0; index < change_count; ++index) {
        auto &item = output[index];
        item.device_id = changes[index].device_id;
        item.endpoint_id = changes[index].endpoint_id;
        snprintf(item.cluster, sizeof(item.cluster), "%s",
                 changes[index].cluster);
        snprintf(item.property, sizeof(item.property), "%s",
                 changes[index].property);
        item.previous_value_json =
            cJSON_PrintUnformatted(changes[index].previous_value);
        item.value_json = cJSON_PrintUnformatted(changes[index].value);
        if (!item.previous_value_json || !item.value_json) {
            complete = false;
            break;
        }
    }

    if (complete) {
        dispatch_listeners(manager, &fabric_manager_listener_t::properties_changed,
                           output, change_count);
    }
    for (size_t index = 0; index < change_count; ++index) {
        cJSON_free(const_cast<char *>(output[index].previous_value_json));
        cJSON_free(const_cast<char *>(output[index].value_json));
    }
    free(output);
}

esp_err_t fabric_manager_create(fabric_core_handle_t core, fabric_manager_handle_t *out_manager)
{
    ESP_RETURN_ON_FALSE(core && out_manager, ESP_ERR_INVALID_ARG, kTag,
                        "invalid create arguments");
    *out_manager = nullptr;
    auto manager =
        static_cast<fabric_manager_handle_t>(calloc(1, sizeof(struct fabric_manager)));
    ESP_RETURN_ON_FALSE(manager, ESP_ERR_NO_MEM, kTag, "allocate device manager");
    manager->core = core;
    manager->next_listener_id = 1;
    manager->state_mutex = xSemaphoreCreateMutex();
    manager->listener_mutex = xSemaphoreCreateMutex();
    if (!manager->state_mutex || !manager->listener_mutex) {
        fabric_manager_delete(manager);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error =
        nvs_open(FABRIC_MANAGER_NVS_NAMESPACE, NVS_READWRITE, &manager->nvs);
    if (error != ESP_OK) {
        fabric_manager_delete(manager);
        return error;
    }
    error =
        data_model::store::load(manager, &manager->set.devices, &manager->set.device_count);
    if (error == ESP_ERR_INVALID_VERSION) {
        nvs_erase_key(manager->nvs, FABRIC_MANAGER_NVS_KEY);
        nvs_commit(manager->nvs);
        manager->next_device_id = 1;
        error = ESP_OK;
    }
    if (error != ESP_OK) {
        fabric_manager_delete(manager);
        return error;
    }
    const fabric_core_private_listener listener = {
        .node_set_changed = on_node_set_changed,
        .subscription_changed = on_subscription_changed,
        .report_begin = data_model::report_begin,
        .attribute_reported = data_model::attribute_reported,
        .report_end = data_model::report_end,
        .user_ctx = manager,
    };
    error = fabric_core_private_set_listener(core, &listener);
    if (error != ESP_OK) {
        fabric_manager_delete(manager);
        return error;
    }
    *out_manager = manager;
    return ESP_OK;
}

esp_err_t fabric_manager_delete(fabric_manager_handle_t manager)
{
    if (!manager)
        return ESP_ERR_INVALID_ARG;
    if (manager->listener_mutex) {
        data_model::lock_guard lock(manager->listener_mutex);
        if (manager->listener_calls)
            return ESP_ERR_INVALID_STATE;
        for (const auto &entry : manager->listeners) {
            if (entry.drained)
                return ESP_ERR_INVALID_STATE;
        }
    }
    if (manager->core) {
        const esp_err_t error =
            fabric_core_private_clear_listener(manager->core, manager);
        if (error != ESP_OK && error != ESP_ERR_NOT_FOUND)
            return error;
    }
    while (manager->reports) {
        auto *transaction = manager->reports;
        manager->reports = transaction->next;
        free(transaction);
    }
    data_model::model::free(&manager->set);
    if (manager->nvs)
        nvs_close(manager->nvs);
    if (manager->listener_mutex)
        vSemaphoreDelete(manager->listener_mutex);
    if (manager->state_mutex)
        vSemaphoreDelete(manager->state_mutex);
    free(manager);
    return ESP_OK;
}

esp_err_t fabric_manager_add_listener(fabric_manager_handle_t manager,
                               const fabric_manager_listener_t *listener,
                               fabric_manager_listener_id_t *out_listener_id)
{
    ESP_RETURN_ON_FALSE(manager && listener && out_listener_id,
                        ESP_ERR_INVALID_ARG, kTag, "invalid listener");
    data_model::lock_guard lock(manager->listener_mutex);
    data_model::listener *slot = nullptr;
    for (auto &entry : manager->listeners) {
        if (entry.id == 0) {
            slot = &entry;
            break;
        }
    }
    if (!slot || manager->next_listener_id == 0)
        return ESP_ERR_NO_MEM;
    slot->id = manager->next_listener_id++;
    slot->value = *listener;
    *out_listener_id = slot->id;
    return ESP_OK;
}

esp_err_t fabric_manager_remove_listener(fabric_manager_handle_t manager,
                                  fabric_manager_listener_id_t listener_id)
{
    ESP_RETURN_ON_FALSE(manager && listener_id != 0, ESP_ERR_INVALID_ARG, kTag,
                        "invalid listener id");
    SemaphoreHandle_t drained = nullptr;
    {
        data_model::lock_guard lock(manager->listener_mutex);
        auto *entry = find_listener(manager, listener_id);
        if (!entry || entry->removing)
            return ESP_ERR_NOT_FOUND;
        if (entry->active_calls == 0) {
            *entry = {};
            return ESP_OK;
        }

        // A callback must never wait for another callback (including an outer
        // invocation on its own task). Only a sole self-invocation may retire.
        const TaskHandle_t task = xTaskGetCurrentTaskHandle();
        for (auto *call = manager->listener_calls; call; call = call->next) {
            if (call->task != task)
                continue;
            if (call->listener_id != listener_id || entry->active_calls != 1)
                return ESP_ERR_INVALID_STATE;
            entry->removing = true;
            return ESP_OK;
        }

        drained = xSemaphoreCreateBinary();
        if (!drained)
            return ESP_ERR_NO_MEM;
        entry->drained = drained;
        entry->removing = true;
    }
    const BaseType_t taken = xSemaphoreTake(drained, portMAX_DELAY);
    assert(taken == pdTRUE);
    (void)taken;
    {
        data_model::lock_guard lock(manager->listener_mutex);
        auto *entry = find_listener(manager, listener_id);
        assert(entry && entry->active_calls == 0);
        *entry = {};
    }
    vSemaphoreDelete(drained);
    return ESP_OK;
}

esp_err_t fabric_manager_list_devices_json(fabric_manager_handle_t manager, char **out_json)
{
    ESP_RETURN_ON_FALSE(manager && out_json, ESP_ERR_INVALID_ARG, kTag,
                        "invalid list output");
    *out_json = nullptr;
    {
        data_model::lock_guard lock(manager->state_mutex);
        *out_json = data_model::json::list(manager->set);
    }
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t fabric_manager_get_device_json(fabric_manager_handle_t manager,
                                  fabric_manager_device_id_t device_id,
                                  char **out_json)
{
    ESP_RETURN_ON_FALSE(manager && device_id != 0 && out_json, ESP_ERR_INVALID_ARG,
                        kTag, "invalid detail output");
    *out_json = nullptr;
    bool found = false;
    {
        data_model::lock_guard lock(manager->state_mutex);
        const auto *device = find_device(manager->set, device_id);
        found = device != nullptr;
        if (device)
            *out_json = data_model::json::detail(manager->set, *device);
    }
    if (!found)
        return ESP_ERR_NOT_FOUND;
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

void fabric_manager_free_json(char *json) { cJSON_free(json); }

esp_err_t fabric_manager_set_device_identity(fabric_manager_handle_t manager,
                                      fabric_manager_device_id_t device_id,
                                      const char *name, const char *room)
{
    ESP_RETURN_ON_FALSE(manager && device_id != 0 && name && room,
                        ESP_ERR_INVALID_ARG, kTag, "invalid identity");
    ESP_RETURN_ON_FALSE(strnlen(name, FABRIC_MANAGER_NAME_MAX) < FABRIC_MANAGER_NAME_MAX &&
                            strnlen(room, FABRIC_MANAGER_ROOM_MAX) < FABRIC_MANAGER_ROOM_MAX,
                        ESP_ERR_INVALID_SIZE, kTag, "identity too long");
    esp_err_t error = ESP_OK;
    {
        data_model::lock_guard lock(manager->state_mutex);
        auto *device = find_device(manager->set, device_id);
        if (!device)
            return ESP_ERR_NOT_FOUND;
        char previous_name[FABRIC_MANAGER_NAME_MAX] = {};
        char previous_room[FABRIC_MANAGER_ROOM_MAX] = {};
        memcpy(previous_name, device->name, sizeof(previous_name));
        memcpy(previous_room, device->room, sizeof(previous_room));
        snprintf(device->name, sizeof(device->name), "%s", name);
        snprintf(device->room, sizeof(device->room), "%s", room);
        error = data_model::store::save(manager, manager->set.devices,
                                  manager->set.device_count, manager->next_device_id);
        if (error != ESP_OK) {
            memcpy(device->name, previous_name, sizeof(device->name));
            memcpy(device->room, previous_room, sizeof(device->room));
        }
    }
    if (error == ESP_OK)
        data_model::dispatch_device_set_changed(manager);
    return error;
}

esp_err_t fabric_manager_invoke_action(
    fabric_manager_handle_t manager, fabric_manager_device_id_t device_id, uint16_t endpoint_id,
    const char *cluster_key, const char *action_key, const char *arguments_json,
    fabric_core_request_id_t *out_request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(manager && device_id != 0 && endpoint_id != 0 &&
                            cluster_key && action_key && arguments_json &&
                            out_request_id && completion_cb,
                        ESP_ERR_INVALID_ARG, kTag, "invalid action");
    cJSON *arguments = cJSON_Parse(arguments_json);
    if (!cJSON_IsObject(arguments)) {
        cJSON_Delete(arguments);
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t node_id = 0;
    const data_model::cluster_adapter *adapter = nullptr;
    data_model::cluster_state cluster_state = {};
    {
        data_model::lock_guard lock(manager->state_mutex);
        adapter = find_action_cluster(manager->set, device_id, endpoint_id,
                                      cluster_key, &node_id, &cluster_state);
    }
    if (!adapter || !adapter->invoke_action) {
        cJSON_Delete(arguments);
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t error = adapter->invoke_action(
        manager->core, node_id, endpoint_id, &cluster_state, action_key, arguments,
        out_request_id, completion_cb, user_ctx);
    cJSON_Delete(arguments);
    return error;
}

esp_err_t fabric_manager_remove_device(fabric_manager_handle_t manager,
                                fabric_manager_device_id_t device_id, bool force,
                                fabric_core_request_id_t *out_request_id,
                                fabric_core_request_completion_cb_t completion_cb,
                                void *user_ctx)
{
    ESP_RETURN_ON_FALSE(manager && device_id != 0 && out_request_id &&
                            completion_cb,
                        ESP_ERR_INVALID_ARG, kTag, "invalid removal");
    uint64_t node_id = 0;
    {
        data_model::lock_guard lock(manager->state_mutex);
        const auto *device = find_device(manager->set, device_id);
        if (device)
            node_id = device->node_id;
    }
    if (!node_id)
        return ESP_ERR_NOT_FOUND;
    return fabric_core_remove_node(manager->core, node_id, force, out_request_id,
                               completion_cb, user_ctx);
}
