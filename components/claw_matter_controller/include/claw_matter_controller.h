/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/**
 * @file
 * @brief Agent-facing adapter for Local Fabric endpoint/cluster devices.
 *
 * Manager remains authoritative. Operations and committed changes use explicit
 * device, endpoint, cluster, and member coordinates. Subscriptions are
 * transient and are never persisted by this component.
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "fabric_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t claw_matter_subscription_id_t;

/** Optional exact-match filter. Every null field is a wildcard. */
typedef struct
{
    const fabric_manager_device_id_t *device_id;
    const uint16_t *endpoint_id;
    const char *cluster;
    const char *property;
} claw_matter_event_filter_t;

/** One copied, authoritative property change. */
typedef struct
{
    fabric_manager_device_id_t device_id;
    uint16_t endpoint_id;
    const char *cluster;
    const char *property;
    const char *previous_value_json;
    const char *value_json;
} claw_matter_change_t;

/** Callback-scoped pointers to changes matching one subscription. */
typedef struct
{
    const claw_matter_change_t *const *changes;
    size_t change_count;
} claw_matter_change_batch_t;

typedef void (*claw_matter_event_callback_t)(
    const claw_matter_change_batch_t *batch, void *user_ctx);

esp_err_t claw_matter_controller_start(fabric_manager_handle_t manager);
/**
 * Stops delivery and waits for the worker to exit. Calling from an event
 * callback returns ESP_ERR_INVALID_STATE without changing controller state.
 */
esp_err_t claw_matter_controller_stop(void);

/**
 * Copies a transient subscription. Registrations made from an event callback
 * begin with subsequent batches. IDs are not reused during one controller run.
 */
esp_err_t
claw_matter_subscribe(const claw_matter_event_filter_t *filter,
                      claw_matter_event_callback_t callback, void *user_ctx,
                      claw_matter_subscription_id_t *out_subscription_id);
/**
 * Prevents further invocations and waits for an active callback when called
 * outside the worker. From a callback, supports self-removal and removal of a
 * later subscription in the same batch. A self-removing callback must keep
 * its user_ctx valid until it returns. A removed subscription's pending turn
 * is skipped even if its slot is reused by a new registration.
 */
esp_err_t
claw_matter_unsubscribe(claw_matter_subscription_id_t subscription_id);
uint32_t claw_matter_dropped_batch_count(void);

/** Returned JSON uses cJSON allocation and is released with cJSON_free(). */
esp_err_t claw_matter_list_devices_json(char **out_json);
esp_err_t claw_matter_get_device_json(fabric_manager_device_id_t device_id,
                                      char **out_json);
esp_err_t claw_matter_get_property_json(fabric_manager_device_id_t device_id,
                                        uint16_t endpoint_id,
                                        const char *cluster,
                                        const char *property, char **out_json);
esp_err_t claw_matter_invoke_action(fabric_manager_device_id_t device_id,
                                    uint16_t endpoint_id, const char *cluster,
                                    const char *action,
                                    const char *arguments_json,
                                    fabric_core_request_id_t *out_request_id);

/** Registers the operation-only `matter` Lua module with app_claw. */
esp_err_t claw_matter_lua_register_module(void);

#ifdef __cplusplus
}
#endif
