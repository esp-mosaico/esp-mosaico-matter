/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/**
 * @file
 * @brief Resolved Local Fabric devices expressed as endpoints and clusters.
 *
 * Matter topology and node addresses are private to the device manager. Public
 * operations use a stable device id plus an endpoint/cluster/member coordinate.
 */

#include "esp_err.h"
#include "fabric_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FABRIC_MANAGER_NAME_MAX 64U
#define FABRIC_MANAGER_ROOM_MAX 64U
#define FABRIC_MANAGER_CLUSTER_KEY_MAX 40U
#define FABRIC_MANAGER_MEMBER_KEY_MAX 40U

/** Opaque runtime handle. */
typedef struct fabric_manager *fabric_manager_handle_t;
/** Stable consumer identity; it intentionally does not encode Matter topology.
 */
typedef uint32_t fabric_manager_device_id_t;
/** Listener registration token. */
typedef uint32_t fabric_manager_listener_id_t;

/** One authoritative committed property change. */
typedef struct
{
    fabric_manager_device_id_t device_id;
    uint16_t endpoint_id;
    char cluster[FABRIC_MANAGER_CLUSTER_KEY_MAX];
    char property[FABRIC_MANAGER_MEMBER_KEY_MAX];
    const char *previous_value_json; /**< Callback-scoped JSON value. */
    const char *value_json;          /**< Callback-scoped JSON value. */
} fabric_manager_property_change_t;

/**
 * Synchronous device-manager notifications, called without Manager locks held.
 *
 * Matter notifications run on the CHIP task. Identity changes notify on the
 * task calling fabric_manager_set_device_identity(). Callbacks may therefore overlap
 * across tasks and may be nested; protect shared user_ctx data accordingly.
 * Keep callbacks short and do not wait for other callbacks to finish.
 * Registration and removal from a callback are supported as described below.
 */
typedef struct
{
    /** Device identity, membership, presentation, or reachability changed. */
    void (*device_set_changed)(fabric_manager_handle_t manager, void *user_ctx);
    /** One or more property values were atomically committed. */
    void (*properties_changed)(fabric_manager_handle_t manager,
                               const fabric_manager_property_change_t *changes,
                               size_t change_count, void *user_ctx);
    void *user_ctx;
} fabric_manager_listener_t;

/** Creates and attaches Manager to a stopped Local Fabric Core. */
esp_err_t fabric_manager_create(fabric_core_handle_t core, fabric_manager_handle_t *out_manager);
/**
 * Detaches and destroys Manager after the core has stopped and other API callers
 * have quiesced. Returns ESP_ERR_INVALID_STATE while callbacks or listener
 * removal are in progress, including deletion attempted from a callback.
 */
esp_err_t fabric_manager_delete(fabric_manager_handle_t manager);

/**
 * Copies a bounded synchronous listener. Registration from a callback is safe;
 * the new listener starts with subsequent notifications, including nested ones.
 */
esp_err_t fabric_manager_add_listener(fabric_manager_handle_t manager,
                               const fabric_manager_listener_t *listener,
                               fabric_manager_listener_id_t *out_listener_id);
/**
 * Removes a listener, preventing further invocations and waiting for in-flight
 * callbacks before returning ESP_OK. A sole self-invocation may unregister
 * itself; its user_ctx must remain valid until that invocation returns.
 *
 * From a callback, returns ESP_ERR_INVALID_STATE without removing the listener
 * if removal would wait for another invocation. Outside callbacks, waiting
 * needs a temporary semaphore and may return ESP_ERR_NO_MEM without removal.
 * ESP_ERR_NOT_FOUND also covers a removal already in progress; only ESP_OK
 * guarantees completion. Do not hold resources needed by the callbacks while
 * waiting. Callback-scoped arguments remain valid only until callback return.
 */
esp_err_t fabric_manager_remove_listener(fabric_manager_handle_t manager,
                                  fabric_manager_listener_id_t listener_id);

/**
 * Allocates the authoritative device-summary document.
 *
 * The result has shape `{ "devices": [...] }` and must be released with
 * `fabric_manager_free_json()`.
 */
esp_err_t fabric_manager_list_devices_json(fabric_manager_handle_t manager, char **out_json);

/**
 * Allocates one endpoint/cluster device-detail document.
 *
 * The result must be released with `fabric_manager_free_json()`.
 */
esp_err_t fabric_manager_get_device_json(fabric_manager_handle_t manager,
                                  fabric_manager_device_id_t device_id,
                                  char **out_json);

/** Releases JSON returned by Manager. NULL is accepted. */
void fabric_manager_free_json(char *json);

/** Updates consumer-owned presentation identity and persists it atomically. */
esp_err_t fabric_manager_set_device_identity(fabric_manager_handle_t manager,
                                      fabric_manager_device_id_t device_id,
                                      const char *name, const char *room);

/**
 * Invokes one advertised action by explicit coordinate.
 *
 * `arguments_json` must contain exactly the arguments advertised by the action.
 * Successful command completion does not mutate property state; reports remain
 * authoritative.
 */
esp_err_t fabric_manager_invoke_action(fabric_manager_handle_t manager,
                                fabric_manager_device_id_t device_id,
                                uint16_t endpoint_id, const char *cluster,
                                const char *action, const char *arguments_json,
                                fabric_core_request_id_t *out_request_id,
                                fabric_core_request_completion_cb_t completion_cb,
                                void *user_ctx);

/** Removes the Matter node containing the resolved device. */
esp_err_t fabric_manager_remove_device(fabric_manager_handle_t manager,
                                fabric_manager_device_id_t device_id, bool force,
                                fabric_core_request_id_t *out_request_id,
                                fabric_core_request_completion_cb_t completion_cb,
                                void *user_ctx);

#ifdef __cplusplus
}
#endif
