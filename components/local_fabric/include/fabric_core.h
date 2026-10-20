/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/**
 * @file
 * @brief Public lifecycle, Matter report, and request API for Local Fabric.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FABRIC_CORE_TEXT_MAX 256U

/** Opaque Local Fabric Core handle. */
typedef struct fabric_core *fabric_core_handle_t;
/** Nonzero identifier for an accepted asynchronous request. */
typedef uint32_t fabric_core_request_id_t;
/** Nonzero identifier for a registered public listener. */
typedef uint32_t fabric_core_listener_id_t;

/** Matter node and endpoint address. */
typedef struct {
    uint64_t node_id;     /**< Commissioned Matter node identifier. */
    uint16_t endpoint_id; /**< Matter endpoint identifier. */
} fabric_core_endpoint_t;

/** Matter attribute path. */
typedef struct {
    fabric_core_endpoint_t endpoint; /**< Node and endpoint. */
    uint32_t cluster_id;         /**< Matter cluster identifier. */
    uint32_t attribute_id;       /**< Matter attribute identifier. */
} fabric_core_attribute_path_t;

/** Matter command path. */
typedef struct {
    fabric_core_endpoint_t endpoint; /**< Node and endpoint. */
    uint32_t cluster_id;         /**< Matter cluster identifier. */
    uint32_t command_id;         /**< Matter command identifier. */
} fabric_core_command_path_t;

/** Representation used at the public raw Matter value boundary. */
typedef enum {
    FABRIC_CORE_VALUE_NULL = 0,
    FABRIC_CORE_VALUE_BOOL,
    FABRIC_CORE_VALUE_SIGNED,
    FABRIC_CORE_VALUE_UNSIGNED,
    FABRIC_CORE_VALUE_FLOAT,
    FABRIC_CORE_VALUE_STRING,
    FABRIC_CORE_VALUE_OCTETS,
    FABRIC_CORE_VALUE_JSON,
} fabric_core_value_type_t;

/** Bounded raw Matter value returned by public core APIs. */
typedef struct {
    fabric_core_value_type_t type; /**< Active representation. */
    bool truncated; /**< Whether text exceeded @ref FABRIC_CORE_TEXT_MAX. */
    union {
        bool boolean;
        int64_t signed_value;
        uint64_t unsigned_value;
        double float_value;
    } data;                      /**< Scalar storage selected by @ref type. */
    char text[FABRIC_CORE_TEXT_MAX]; /**< String, octets, or JSON representation. */
} fabric_core_value_t;

/** State of a node's authoritative wildcard subscription. */
typedef enum {
    FABRIC_CORE_SUBSCRIPTION_CONNECTING = 0,
    FABRIC_CORE_SUBSCRIPTION_ACTIVE,
    FABRIC_CORE_SUBSCRIPTION_BACKOFF,
    FABRIC_CORE_SUBSCRIPTION_TERMINATED,
} fabric_core_subscription_state_t;

/** Subscription-state notification for one commissioned node. */
typedef struct {
    uint64_t node_id;
    fabric_core_subscription_state_t state;
    esp_err_t cause; /**< ESP_OK or the transition's error cause. */
} fabric_core_subscription_update_t;

/** Public decoded attribute report, valid only during its callback. */
typedef struct {
    fabric_core_attribute_path_t path;
    fabric_core_value_t value;
    esp_err_t status;
} fabric_core_attribute_report_t;

/** Public Local Fabric Core listener callbacks. */
typedef struct {
    /** Called when a node subscription changes state. */
    void (*subscription_changed)(fabric_core_handle_t core,
                                 const fabric_core_subscription_update_t *update,
                                 void *user_ctx);
    /** Called before attributes belonging to one report are delivered. */
    void (*report_begin)(fabric_core_handle_t core, uint64_t node_id,
                         void *user_ctx);
    /** Called for each attribute in the active report. */
    void (*attribute_reported)(fabric_core_handle_t core,
                               const fabric_core_attribute_report_t *report,
                               void *user_ctx);
    /** Called once when the active report finishes. */
    void (*report_end)(fabric_core_handle_t core, uint64_t node_id, esp_err_t error,
                       void *user_ctx);
    void *user_ctx; /**< Opaque callback context; NULL is valid. */
} fabric_core_listener_t;

/** Synchronous provider for Matter Wi-Fi commissioning credentials. */
typedef esp_err_t (*fabric_core_wifi_credentials_cb_t)(char *ssid, size_t ssid_size,
                                                   char *password,
                                                   size_t password_size,
                                                   void *user_ctx);

/**
 * Synchronous provider for a Matter Thread operational dataset.
 *
 * Return ESP_ERR_NOT_FOUND when Thread is optional but currently unavailable;
 * commissioning then continues without a Thread dataset.
 */
typedef esp_err_t (*fabric_core_thread_dataset_cb_t)(uint8_t *buffer,
                                                 size_t capacity,
                                                 size_t *length,
                                                 void *user_ctx);

/** Configuration copied by @ref fabric_core_create. */
typedef struct {
    uint16_t max_nodes; /**< Maximum commissioned nodes owned by the core. */
    uint16_t subscription_interval_s_min; /**< Minimum report interval. */
    uint16_t subscription_interval_s_max; /**< Maximum report interval. */
    fabric_core_wifi_credentials_cb_t get_wifi_credentials;
    void *wifi_credentials_ctx;
    fabric_core_thread_dataset_cb_t get_thread_dataset;
    void *thread_dataset_ctx;
    /** Skip PAA trust-chain and CD verification; retain device proof and CSR checks. */
    bool skip_paa_and_cd_checks;
} fabric_core_config_t;

/** Kind of asynchronous core request that completed. */
typedef enum {
    FABRIC_CORE_OPERATION_COMMISSION = 0,
    FABRIC_CORE_OPERATION_READ_ATTRIBUTE,
    FABRIC_CORE_OPERATION_WRITE_ATTRIBUTE,
    FABRIC_CORE_OPERATION_INVOKE_COMMAND,
    FABRIC_CORE_OPERATION_REMOVE_NODE,
} fabric_core_operation_t;

/** Callback-scoped result of one asynchronous core request. */
typedef struct {
    fabric_core_request_id_t request_id;
    fabric_core_operation_t operation;
    esp_err_t error;
    uint64_t node_id;
    fabric_core_value_t value; /**< Read result; otherwise null-valued. */
} fabric_core_request_result_t;

/** Callback invoked exactly once for each accepted asynchronous request. */
typedef void (*fabric_core_request_completion_cb_t)(
    fabric_core_handle_t core, const fabric_core_request_result_t *result,
    void *user_ctx);

/** Creates a stopped controller and initializes its CHIP state owner. */
esp_err_t fabric_core_create(const fabric_core_config_t *config,
                         fabric_core_handle_t *out_core);
/** Starts subscriptions and begins accepting asynchronous requests. */
esp_err_t fabric_core_start(fabric_core_handle_t core);
/** Stops new work and waits for accepted requests and callbacks to drain. */
esp_err_t fabric_core_stop(fabric_core_handle_t core);
/** Deletes a stopped core. Lifecycle APIs cannot run from a core callback. */
esp_err_t fabric_core_delete(fabric_core_handle_t core);

/** Registers a listener and returns its nonzero identifier. */
esp_err_t fabric_core_add_listener(fabric_core_handle_t core,
                               const fabric_core_listener_t *listener,
                               fabric_core_listener_id_t *out_listener_id);
/** Synchronously removes a listener outside its callbacks. */
esp_err_t fabric_core_remove_listener(fabric_core_handle_t core,
                                  fabric_core_listener_id_t listener_id);
/** Lists commissioned node identifiers using count-then-fill semantics. */
esp_err_t fabric_core_get_node_ids(fabric_core_handle_t core, uint64_t *node_ids,
                               size_t capacity, size_t *out_count);
/** Starts asynchronous commissioning from a setup code. */
esp_err_t
fabric_core_start_commissioning(fabric_core_handle_t core, const char *setup_code,
                            fabric_core_request_id_t *out_request_id,
                            fabric_core_request_completion_cb_t completion_cb,
                            void *user_ctx);
/** Cancels an active commissioning attempt. */
esp_err_t fabric_core_cancel_commissioning(fabric_core_handle_t core);
/** Submits an asynchronous raw Matter attribute read. */
esp_err_t fabric_core_read_attribute(fabric_core_handle_t core,
                                 const fabric_core_attribute_path_t *path,
                                 fabric_core_request_id_t *out_request_id,
                                 fabric_core_request_completion_cb_t completion_cb,
                                 void *user_ctx);
/** Submits an asynchronous raw Matter attribute write. */
esp_err_t fabric_core_write_attribute(fabric_core_handle_t core,
                                  const fabric_core_attribute_path_t *path,
                                  const fabric_core_value_t *value,
                                  fabric_core_request_id_t *out_request_id,
                                  fabric_core_request_completion_cb_t completion_cb,
                                  void *user_ctx);
/** Submits an asynchronous raw Matter command invocation. */
esp_err_t fabric_core_invoke_command(fabric_core_handle_t core,
                                 const fabric_core_command_path_t *path,
                                 const char *arguments_json,
                                 fabric_core_request_id_t *out_request_id,
                                 fabric_core_request_completion_cb_t completion_cb,
                                 void *user_ctx);
/** Removes a commissioned node asynchronously. */
esp_err_t fabric_core_remove_node(fabric_core_handle_t core, uint64_t node_id,
                              bool force, fabric_core_request_id_t *out_request_id,
                              fabric_core_request_completion_cb_t completion_cb,
                              void *user_ctx);
/** Removes all Local Fabric state using the platform factory-reset path. */
esp_err_t fabric_core_factory_reset(fabric_core_handle_t core);
/** Immediately retries a node subscription currently in backoff. */
esp_err_t fabric_core_retry_node_subscription(fabric_core_handle_t core,
                                          uint64_t node_id);

#ifdef __cplusplus
}
#endif
