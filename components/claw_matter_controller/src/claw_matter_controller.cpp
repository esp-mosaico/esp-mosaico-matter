/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_matter_private.h"

#include "esp_check.h"

#include <cstdlib>
#include <cstring>

namespace claw_matter
{

controller_state *g_controller;

namespace
{

constexpr char kTag[] = "claw_matter";
constexpr uint32_t kWorkerStackBytes = 4096;
constexpr UBaseType_t kWorkerPriority = 5;

bool matches(const subscription &entry, const claw_matter_change_t &change)
{
    return (!entry.has_device_id || entry.device_id == change.device_id) &&
           (!entry.has_endpoint_id ||
            entry.endpoint_id == change.endpoint_id) &&
           (!entry.has_cluster || !strcmp(entry.cluster, change.cluster)) &&
           (!entry.has_property || !strcmp(entry.property, change.property));
}

void dispatch(controller_state *state, const queued_event &event)
{
    xSemaphoreTakeRecursive(state->dispatch_mutex, portMAX_DELAY);
    claw_matter_subscription_id_t
        snapshot[CONFIG_CLAW_MATTER_MAX_EVENT_SUBSCRIPTIONS] = {};
    size_t subscription_count = 0;
    xSemaphoreTake(state->subscription_mutex, portMAX_DELAY);
    for (const auto &entry : state->subscriptions) {
        if (entry.id != 0)
            snapshot[subscription_count++] = entry.id;
    }
    xSemaphoreGive(state->subscription_mutex);

    for (size_t subscription_index = 0; subscription_index < subscription_count;
         ++subscription_index) {
        subscription entry = {};
        xSemaphoreTake(state->subscription_mutex, portMAX_DELAY);
        for (const auto &registered : state->subscriptions) {
            if (registered.id == snapshot[subscription_index]) {
                entry = registered;
                break;
            }
        }
        xSemaphoreGive(state->subscription_mutex);
        if (entry.id == 0)
            continue;
        // dispatch_mutex keeps external unsubscribe from completing between
        // this lookup and the callback. Reentrant removals affect later IDs.
        const claw_matter_change_t
            *matched[CONFIG_CLAW_MATTER_EVENT_MAX_CHANGES] = {};
        size_t matched_count = 0;
        for (size_t change_index = 0; change_index < event.change_count;
             ++change_index) {
            const auto &change = event.changes[change_index].value;
            if (matches(entry, change))
                matched[matched_count++] = &change;
        }
        if (matched_count != 0) {
            const claw_matter_change_batch_t batch = {
                .changes = matched,
                .change_count = matched_count,
            };
            entry.callback(&batch, entry.user_ctx);
        }
    }
    xSemaphoreGiveRecursive(state->dispatch_mutex);
}

void worker(void *argument)
{
    auto *state = static_cast<controller_state *>(argument);
    while (true) {
        queued_event *event = nullptr;
        if (xQueueReceive(state->event_queue, &event, portMAX_DELAY) !=
            pdTRUE) {
            continue;
        }
        if (!event)
            break;
        dispatch(state, *event);
        free(event);
    }
    xTaskNotifyGive(state->stop_waiter);
    vTaskDelete(nullptr);
}

void drop(controller_state *state, queued_event *event)
{
    free(event);
    state->dropped_batches.fetch_add(1);
}

void enqueue(fabric_manager_handle_t, const fabric_manager_property_change_t *changes,
             size_t change_count, void *user_ctx)
{
    auto *state = static_cast<controller_state *>(user_ctx);
    if (!state || !changes || change_count == 0)
        return;
    if (change_count > CONFIG_CLAW_MATTER_EVENT_MAX_CHANGES) {
        state->dropped_batches.fetch_add(1);
        return;
    }
    auto *event = static_cast<queued_event *>(calloc(1, sizeof(queued_event)));
    if (!event) {
        state->dropped_batches.fetch_add(1);
        return;
    }
    event->change_count = change_count;
    for (size_t index = 0; index < change_count; ++index) {
        const auto &source = changes[index];
        auto &copy = event->changes[index];
        if (!source.previous_value_json || !source.value_json ||
            strnlen(source.previous_value_json,
                    sizeof(copy.previous_value_json)) >=
                sizeof(copy.previous_value_json) ||
            strnlen(source.value_json, sizeof(copy.value_json)) >=
                sizeof(copy.value_json)) {
            drop(state, event);
            return;
        }
        copy.value.device_id = source.device_id;
        copy.value.endpoint_id = source.endpoint_id;
        strlcpy(copy.cluster, source.cluster, sizeof(copy.cluster));
        strlcpy(copy.property, source.property, sizeof(copy.property));
        strlcpy(copy.previous_value_json, source.previous_value_json,
                sizeof(copy.previous_value_json));
        strlcpy(copy.value_json, source.value_json, sizeof(copy.value_json));
        copy.value.cluster = copy.cluster;
        copy.value.property = copy.property;
        copy.value.previous_value_json = copy.previous_value_json;
        copy.value.value_json = copy.value_json;
    }
    if (xQueueSend(state->event_queue, &event, 0) != pdTRUE) {
        drop(state, event);
    }
}

bool valid_key(const char *value, size_t capacity)
{
    return !value || (value[0] != '\0' && strnlen(value, capacity) < capacity);
}

bool valid_filter(const claw_matter_event_filter_t *filter)
{
    if (!filter)
        return true;
    if (filter->device_id && *filter->device_id == 0)
        return false;
    if (filter->endpoint_id && *filter->endpoint_id == 0)
        return false;
    return valid_key(filter->cluster, FABRIC_MANAGER_CLUSTER_KEY_MAX) &&
           valid_key(filter->property, FABRIC_MANAGER_MEMBER_KEY_MAX);
}

void copy_filter(subscription &slot, const claw_matter_event_filter_t *filter)
{
    if (!filter)
        return;
    slot.has_device_id = filter->device_id;
    slot.has_endpoint_id = filter->endpoint_id;
    slot.has_cluster = filter->cluster;
    slot.has_property = filter->property;
    if (slot.has_device_id)
        slot.device_id = *filter->device_id;
    if (slot.has_endpoint_id)
        slot.endpoint_id = *filter->endpoint_id;
    if (slot.has_cluster)
        strlcpy(slot.cluster, filter->cluster, sizeof(slot.cluster));
    if (slot.has_property)
        strlcpy(slot.property, filter->property, sizeof(slot.property));
}

void destroy(controller_state *state)
{
    if (!state)
        return;
    if (state->event_queue)
        vQueueDelete(state->event_queue);
    if (state->dispatch_mutex)
        vSemaphoreDelete(state->dispatch_mutex);
    if (state->subscription_mutex)
        vSemaphoreDelete(state->subscription_mutex);
    free(state);
}

} // namespace
} // namespace claw_matter

esp_err_t claw_matter_controller_start(fabric_manager_handle_t manager)
{
    using namespace claw_matter;
    ESP_RETURN_ON_FALSE(manager, ESP_ERR_INVALID_ARG, kTag,
                        "device manager is required");
    ESP_RETURN_ON_FALSE(!g_controller, ESP_ERR_INVALID_STATE, kTag,
                        "controller is already started");

    auto *state =
        static_cast<controller_state *>(calloc(1, sizeof(controller_state)));
    ESP_RETURN_ON_FALSE(state, ESP_ERR_NO_MEM, kTag, "allocate controller");
    state->manager = manager;
    state->next_subscription_id = 1;
    state->subscription_mutex = xSemaphoreCreateMutex();
    state->dispatch_mutex = xSemaphoreCreateRecursiveMutex();
    state->event_queue = xQueueCreate(CONFIG_CLAW_MATTER_EVENT_QUEUE_DEPTH,
                                      sizeof(queued_event *));
    if (!state->subscription_mutex || !state->dispatch_mutex ||
        !state->event_queue) {
        destroy(state);
        return ESP_ERR_NO_MEM;
    }

    const fabric_manager_listener_t listener = {
        .device_set_changed = nullptr,
        .properties_changed = enqueue,
        .user_ctx = state,
    };
    esp_err_t error =
        fabric_manager_add_listener(manager, &listener, &state->listener_id);
    if (error != ESP_OK) {
        destroy(state);
        return error;
    }
    if (xTaskCreate(worker, "claw_matter", kWorkerStackBytes, state,
                    kWorkerPriority, &state->worker_task) != pdPASS) {
        fabric_manager_remove_listener(manager, state->listener_id);
        destroy(state);
        return ESP_ERR_NO_MEM;
    }
    g_controller = state;
    return ESP_OK;
}

esp_err_t claw_matter_controller_stop(void)
{
    using namespace claw_matter;
    controller_state *state = g_controller;
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_STATE, kTag,
                        "controller is not started");
    ESP_RETURN_ON_FALSE(xTaskGetCurrentTaskHandle() != state->worker_task,
                        ESP_ERR_INVALID_STATE, kTag,
                        "cannot stop controller from its worker callback");
    g_controller = nullptr;
    esp_err_t error = fabric_manager_remove_listener(state->manager, state->listener_id);
    if (error != ESP_OK) {
        g_controller = state;
        return error;
    }

    queued_event *stop = nullptr;
    state->stop_waiter = xTaskGetCurrentTaskHandle();
    if (xQueueSend(state->event_queue, &stop, portMAX_DELAY) != pdTRUE) {
        g_controller = state;
        return ESP_FAIL;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    queued_event *event = nullptr;
    while (xQueueReceive(state->event_queue, &event, 0) == pdTRUE)
        free(event);
    destroy(state);
    return ESP_OK;
}

esp_err_t
claw_matter_subscribe(const claw_matter_event_filter_t *filter,
                      claw_matter_event_callback_t callback, void *user_ctx,
                      claw_matter_subscription_id_t *out_subscription_id)
{
    using namespace claw_matter;
    ESP_RETURN_ON_FALSE(callback && out_subscription_id, ESP_ERR_INVALID_ARG,
                        kTag, "callback and output are required");
    ESP_RETURN_ON_FALSE(valid_filter(filter), ESP_ERR_INVALID_ARG, kTag,
                        "invalid filter");
    controller_state *state = g_controller;
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_STATE, kTag,
                        "controller is not started");

    xSemaphoreTake(state->subscription_mutex, portMAX_DELAY);
    subscription *slot = nullptr;
    for (auto &entry : state->subscriptions) {
        if (entry.id == 0) {
            slot = &entry;
            break;
        }
    }
    if (!slot || state->next_subscription_id == 0) {
        xSemaphoreGive(state->subscription_mutex);
        return ESP_ERR_NO_MEM;
    }
    slot->id = state->next_subscription_id++;
    copy_filter(*slot, filter);
    slot->callback = callback;
    slot->user_ctx = user_ctx;
    *out_subscription_id = slot->id;
    xSemaphoreGive(state->subscription_mutex);
    return ESP_OK;
}

esp_err_t claw_matter_unsubscribe(claw_matter_subscription_id_t subscription_id)
{
    using namespace claw_matter;
    ESP_RETURN_ON_FALSE(subscription_id != 0, ESP_ERR_INVALID_ARG, kTag,
                        "invalid subscription");
    controller_state *state = g_controller;
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_STATE, kTag,
                        "controller is not started");

    xSemaphoreTakeRecursive(state->dispatch_mutex, portMAX_DELAY);
    xSemaphoreTake(state->subscription_mutex, portMAX_DELAY);
    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (auto &entry : state->subscriptions) {
        if (entry.id == subscription_id) {
            entry = {};
            result = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(state->subscription_mutex);
    xSemaphoreGiveRecursive(state->dispatch_mutex);
    return result;
}

uint32_t claw_matter_dropped_batch_count(void)
{
    return claw_matter::g_controller
               ? claw_matter::g_controller->dropped_batches.load()
               : 0;
}
