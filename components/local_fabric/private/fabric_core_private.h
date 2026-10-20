/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/**
 * @file
 * @brief Private Local Fabric Core state and integration seams.
 */

#include "fabric_core.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include <lib/core/CHIPError.h>
#include <lib/core/TLV.h>

/* Keep persisted namespaces stable across public API renames. */
#define FABRIC_CORE_NVS_NAMESPACE "lf_core"
#define FABRIC_CORE_NODE_IDS_KEY "nodes"
#define FABRIC_CORE_IDENTITY_KEY "identity"
#define FABRIC_CORE_MAX_SETUP_CODE_LENGTH 256U

/** Registered public core listener. */
struct fabric_core_listener_entry {
    fabric_core_listener_id_t id;
    fabric_core_listener_t listener;
};

/** Commissioned node and its owned subscription. */
struct fabric_core_node {
    uint64_t node_id;
    void *subscription;
    fabric_core_subscription_state_t subscription_state;
};

/** Callback-scoped raw TLV attribute report for the private semantic sink. */
struct fabric_core_private_attribute_report {
    fabric_core_attribute_path_t path;
    chip::TLV::TLVReader *data;
    CHIP_ERROR error;
};

/** Single private listener used by the optional semantic device manager. */
struct fabric_core_private_listener {
    /** Called after the commissioned node set changes. */
    void (*node_set_changed)(fabric_core_handle_t core, void *user_ctx);
    /** Called when a node subscription changes state. */
    void (*subscription_changed)(fabric_core_handle_t core,
                                 const fabric_core_subscription_update_t *update,
                                 void *user_ctx);
    /** Opens one report transaction for the given node. */
    void (*report_begin)(fabric_core_handle_t core, uint64_t node_id,
                         void *user_ctx);
    /** Delivers one raw TLV attribute in the corresponding node report. */
    void (*attribute_reported)(fabric_core_handle_t core,
                               const fabric_core_private_attribute_report *report,
                               void *user_ctx);
    /**
     * Closes the active report transaction. snapshot_complete is true for the
     * priming wildcard report after a subscription starts or resubscribes.
     */
    void (*report_end)(fabric_core_handle_t core, uint64_t node_id, esp_err_t error,
                       bool snapshot_complete, void *user_ctx);
    void *user_ctx; /**< Opaque context; NULL is valid. */
};

/** Core lifecycle guarded by the CHIP stack lock. */
enum fabric_core_lifecycle_t {
    FABRIC_CORE_STOPPED,
    FABRIC_CORE_RUNNING,
    FABRIC_CORE_STOPPING,
};

/**
 * @brief Core mutable state owned by the CHIP stack lock.
 *
 * The explicit installation flag is authoritative because a private listener
 * may legally use a NULL callback context.
 */
struct fabric_core {
    fabric_core_config_t config;
    nvs_handle_t nvs;

    fabric_core_lifecycle_t lifecycle;
    uint32_t active_requests;
    SemaphoreHandle_t stop_event;

    fabric_core_request_id_t next_request_id;
    fabric_core_listener_id_t next_listener_id;

    uint64_t controller_node_id;
    uint64_t fabric_id;
    uint64_t next_node_id;

    fabric_core_node *nodes;
    size_t node_count;

    fabric_core_listener_entry listeners[CONFIG_FABRIC_CORE_MAX_LISTENERS];
    fabric_core_private_listener private_listener;
    bool private_listener_installed;
};

/** Finds a commissioned node while the caller holds the CHIP stack lock. */
fabric_core_node *fabric_core_find_node(fabric_core_handle_t core, uint64_t node_id);

/** Begins one drain-counted request while the core is running. */
esp_err_t fabric_core_request_begin(fabric_core_handle_t core);
/** Ends one request and signals a waiting stop when the count reaches zero. */
void fabric_core_request_end(fabric_core_handle_t core);

/** Installs the sole private listener before the core starts. */
esp_err_t
fabric_core_private_set_listener(fabric_core_handle_t core,
                             const fabric_core_private_listener *listener);
/** Clears the matching private listener after the core stops. */
esp_err_t fabric_core_private_clear_listener(fabric_core_handle_t core, void *user_ctx);
/** Notifies the private listener that commissioned nodes changed. */
void fabric_core_private_notify_node_set_changed(fabric_core_handle_t core);

/** Initializes CHIP controller and fabric resources. */
esp_err_t fabric_core_chip_start(fabric_core_handle_t core);
/** Releases CHIP controller resources after requests drain. */
void fabric_core_chip_stop(fabric_core_handle_t core);
/** Loads and validates persisted core identity and node records. */
esp_err_t fabric_core_store_load(fabric_core_handle_t core);
/** Persists the complete commissioned node set. */
esp_err_t fabric_core_store_nodes(fabric_core_handle_t core);
/** Erases persisted Local Fabric Core records. */
esp_err_t fabric_core_store_erase(fabric_core_handle_t core);
/** Starts subscriptions for all commissioned nodes. */
esp_err_t fabric_core_subscriptions_start(fabric_core_handle_t core);
/** Stops and releases all node subscriptions. */
void fabric_core_subscriptions_stop(fabric_core_handle_t core);
/** Starts the authoritative wildcard subscription for one node. */
esp_err_t fabric_core_subscription_start_node(fabric_core_handle_t core,
                                          fabric_core_node *node);
/** Retries one node subscription that is eligible for retry. */
esp_err_t fabric_core_subscription_retry(fabric_core_handle_t core, fabric_core_node *node);

/** Dispatches commissioning through the CHIP integration. */
esp_err_t
fabric_core_chip_start_commissioning(fabric_core_handle_t core, const char *setup_code,
                                 fabric_core_request_id_t request_id,
                                 fabric_core_request_completion_cb_t completion_cb,
                                 void *user_ctx);
/** Cancels commissioning through the CHIP integration. */
esp_err_t fabric_core_chip_cancel_commissioning(fabric_core_handle_t core);
/** Dispatches a raw Matter attribute read. */
esp_err_t fabric_core_chip_read_attribute(
    fabric_core_handle_t core, const fabric_core_attribute_path_t *path,
    fabric_core_request_id_t request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx);
/** Dispatches a raw Matter attribute write. */
esp_err_t fabric_core_chip_write_attribute(
    fabric_core_handle_t core, const fabric_core_attribute_path_t *path,
    const fabric_core_value_t *value, fabric_core_request_id_t request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx);
/** Dispatches a raw Matter command invocation. */
esp_err_t fabric_core_chip_invoke_command(
    fabric_core_handle_t core, const fabric_core_command_path_t *path,
    const char *arguments_json, fabric_core_request_id_t request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx);
/** Dispatches commissioned-node removal through the CHIP integration. */
esp_err_t
fabric_core_chip_remove_node(fabric_core_handle_t core, uint64_t node_id, bool force,
                         fabric_core_request_id_t request_id,
                         fabric_core_request_completion_cb_t completion_cb,
                         void *user_ctx);

/** Maps a CHIP error to the public ESP-IDF error domain. */
esp_err_t fabric_core_from_chip_error(CHIP_ERROR error);
/** Converts one raw TLV value for the bounded public core API. */
CHIP_ERROR fabric_core_decode_tlv_value(chip::TLV::TLVReader &reader,
                                    fabric_core_value_t &value);
