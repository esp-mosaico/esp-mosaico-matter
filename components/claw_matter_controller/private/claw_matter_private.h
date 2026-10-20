/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_matter_controller.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdint>

namespace claw_matter
{

struct copied_change
{
    claw_matter_change_t value;
    char cluster[FABRIC_MANAGER_CLUSTER_KEY_MAX];
    char property[FABRIC_MANAGER_MEMBER_KEY_MAX];
    char previous_value_json[CONFIG_CLAW_MATTER_EVENT_VALUE_JSON_MAX];
    char value_json[CONFIG_CLAW_MATTER_EVENT_VALUE_JSON_MAX];
};

struct queued_event
{
    size_t change_count;
    copied_change changes[CONFIG_CLAW_MATTER_EVENT_MAX_CHANGES];
};

struct subscription
{
    claw_matter_subscription_id_t id;
    bool has_device_id;
    fabric_manager_device_id_t device_id;
    bool has_endpoint_id;
    uint16_t endpoint_id;
    bool has_cluster;
    char cluster[FABRIC_MANAGER_CLUSTER_KEY_MAX];
    bool has_property;
    char property[FABRIC_MANAGER_MEMBER_KEY_MAX];
    claw_matter_event_callback_t callback;
    void *user_ctx;
};

struct controller_state
{
    fabric_manager_handle_t manager;
    fabric_manager_listener_id_t listener_id;
    SemaphoreHandle_t subscription_mutex;
    SemaphoreHandle_t dispatch_mutex;
    QueueHandle_t event_queue;
    TaskHandle_t worker_task;
    TaskHandle_t stop_waiter;
    subscription subscriptions[CONFIG_CLAW_MATTER_MAX_EVENT_SUBSCRIPTIONS];
    claw_matter_subscription_id_t next_subscription_id;
    std::atomic<uint32_t> dropped_batches;
};

extern controller_state *g_controller;

} // namespace claw_matter
