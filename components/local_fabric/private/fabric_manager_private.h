/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/** @file @brief Private endpoint/cluster device-manager model. */

#include "fabric_core_private.h"
#include "fabric_manager.h"

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include <lib/core/CHIPError.h>
#include <lib/core/TLV.h>

#include <stddef.h>
#include <stdint.h>

/* Keep persisted namespaces stable across public API renames. */
#define FABRIC_MANAGER_NVS_NAMESPACE "lf_dmgr"
#define FABRIC_MANAGER_NVS_KEY "state"
#define FABRIC_MANAGER_DEVICE_TYPE_MAX 24U
#define FABRIC_MANAGER_ENDPOINT_NAME_MAX 48U
#define FABRIC_MANAGER_CLUSTER_STATE_MAX 160U
#define FABRIC_MANAGER_REPORT_CLUSTERS_MAX                                            \
    (CONFIG_FABRIC_MANAGER_MAX_ENDPOINTS * CONFIG_FABRIC_MANAGER_MAX_CLUSTERS_PER_ENDPOINT)

namespace data_model
{

class lock_guard final
{
  public:
    explicit lock_guard(SemaphoreHandle_t mutex) : mutex_(mutex)
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    ~lock_guard() { xSemaphoreGive(mutex_); }

    lock_guard(const lock_guard &) = delete;
    lock_guard &operator=(const lock_guard &) = delete;

  private:
    SemaphoreHandle_t mutex_;
};

/** Fixed, trivially-copyable storage privately interpreted by one adapter. */
struct cluster_state
{
    alignas(max_align_t) uint8_t bytes[FABRIC_MANAGER_CLUSTER_STATE_MAX];
};

using decode_attribute_fn = CHIP_ERROR (*)(uint32_t attribute_id,
                                           chip::TLV::TLVReader &reader,
                                           cluster_state *state);
using encode_json_fn = esp_err_t (*)(const cluster_state *state,
                                     cJSON *properties, cJSON *actions);
using invoke_action_fn = esp_err_t (*)(
    fabric_core_handle_t core, uint64_t node_id, uint16_t endpoint_id,
    const cluster_state *state, const char *action, const cJSON *arguments,
    fabric_core_request_id_t *out_request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx);

/**
 * One Matter cluster boundary. The decoder ignores unselected attributes with
 * CHIP_ERROR_NOT_FOUND. The encoder appends the complete public property and
 * action maps. The invoker validates the selected action and exact arguments.
 */
struct cluster_adapter
{
    const char *key;
    uint32_t cluster_id;
    decode_attribute_fn decode_attribute;
    encode_json_fn encode_json;
    invoke_action_fn invoke_action;
};

struct endpoint
{
    size_t device_index;
    uint16_t endpoint_id;
    char name[FABRIC_MANAGER_ENDPOINT_NAME_MAX];
    size_t first_cluster;
    size_t cluster_count;
};

struct cluster
{
    size_t endpoint_index;
    const cluster_adapter *adapter;
    cluster_state state;
};

struct device
{
    fabric_manager_device_id_t id;
    uint64_t node_id;
    uint16_t root_endpoint_id;
    char type[FABRIC_MANAGER_DEVICE_TYPE_MAX];
    char name[FABRIC_MANAGER_NAME_MAX];
    char room[FABRIC_MANAGER_ROOM_MAX];
    bool online;
    size_t first_endpoint;
    size_t endpoint_count;
};

struct device_set
{
    device *devices;
    size_t device_count;
    endpoint *endpoints;
    size_t endpoint_count;
    cluster *clusters;
    size_t cluster_count;
};

struct listener
{
    fabric_manager_listener_id_t id;
    fabric_manager_listener_t value;
    size_t active_calls;
    bool removing;
    SemaphoreHandle_t drained;
};

/** Callback stack frames, linked only while holding listener_mutex. */
struct listener_call
{
    listener_call *next;
    TaskHandle_t task;
    fabric_manager_listener_id_t listener_id;
};

struct reported_device_type
{
    uint16_t endpoint_id;
    uint32_t device_type;
};

struct relationship
{
    uint16_t parent_endpoint_id;
    uint16_t child_endpoint_id;
};

struct reported_endpoint
{
    uint16_t endpoint_id;
    uint32_t cluster_mask;
    char tag_name[FABRIC_MANAGER_ENDPOINT_NAME_MAX];
    bool server_list_reported;
    bool tag_list_reported;
};

struct reported_cluster
{
    uint16_t endpoint_id;
    uint8_t adapter_index;
    cluster_state state;
    bool touched;
};

struct report
{
    report *next;
    uint64_t node_id;
    reported_endpoint endpoints[CONFIG_FABRIC_MANAGER_MAX_ENDPOINTS];
    size_t endpoint_count;
    reported_device_type
        device_types[CONFIG_FABRIC_MANAGER_MAX_ENDPOINTS *
                     CONFIG_FABRIC_MANAGER_MAX_DEVICE_TYPES_PER_ENDPOINT];
    size_t device_type_count;
    relationship relationships[CONFIG_FABRIC_MANAGER_MAX_REPORT_RELATIONSHIPS];
    size_t relationship_count;
    reported_cluster clusters[FABRIC_MANAGER_REPORT_CLUSTERS_MAX];
    size_t cluster_count;
    bool overflowed;
    bool descriptor_invalid;
};

struct group
{
    uint16_t root_endpoint_id;
    char type[FABRIC_MANAGER_DEVICE_TYPE_MAX];
};

struct property_change
{
    fabric_manager_device_id_t device_id;
    uint16_t endpoint_id;
    char cluster[FABRIC_MANAGER_CLUSTER_KEY_MAX];
    char property[FABRIC_MANAGER_MEMBER_KEY_MAX];
    cJSON *previous_value;
    cJSON *value;
};

const cluster_adapter *cluster_at(size_t index);
size_t cluster_count();
const cluster_adapter *find_cluster(uint32_t cluster_id);

namespace topology
{
esp_err_t resolve(const report &value, group *groups, size_t *out_group_count,
                  uint16_t *endpoint_roots);
} // namespace topology

namespace model
{
esp_err_t replace_node(fabric_manager_handle_t manager, const report &value,
                       device_set *out_set);
esp_err_t apply(fabric_manager_handle_t manager, const report &value,
                property_change **out_changes, size_t *out_change_count,
                bool *out_structure_changed);
esp_err_t retain_nodes(fabric_manager_handle_t manager, const uint64_t *node_ids,
                       size_t node_count, device_set *out_set);
void free(device_set *set);
} // namespace model

namespace json
{
char *list(const device_set &set);
char *detail(const device_set &set, const device &value);
esp_err_t add_property(cJSON *properties, const char *key, cJSON *value,
                       const char *unit = nullptr);
cJSON *add_action(cJSON *actions, const char *key);
esp_err_t add_boolean_argument(cJSON *arguments, const char *key);
esp_err_t add_integer_argument(cJSON *arguments, const char *key,
                               int32_t minimum, int32_t maximum,
                               const char *unit = nullptr);
esp_err_t add_number_argument(cJSON *arguments, const char *key, double minimum,
                              double maximum,
                              const char *unit = nullptr);
esp_err_t add_string_argument(cJSON *arguments, const char *key,
                              const char *const *values, size_t value_count);
const cJSON *only(const cJSON *object, const char *key);
bool integer(const cJSON *value, int64_t minimum, int64_t maximum,
             int64_t *out_value);
bool number(const cJSON *value, double minimum, double maximum,
            double *out_value);
} // namespace json

void report_begin(fabric_core_handle_t core, uint64_t node_id, void *user_ctx);
void attribute_reported(fabric_core_handle_t core,
                        const fabric_core_private_attribute_report *value,
                        void *user_ctx);
void report_end(fabric_core_handle_t core, uint64_t node_id, esp_err_t error,
                bool snapshot_complete, void *user_ctx);

void dispatch_device_set_changed(fabric_manager_handle_t manager);
void dispatch_property_changes(fabric_manager_handle_t manager, property_change *changes,
                               size_t change_count);

namespace store
{
esp_err_t load(fabric_manager_handle_t manager, device **out_devices,
               size_t *out_device_count);
esp_err_t save(fabric_manager_handle_t manager, const device *devices,
               size_t device_count, fabric_manager_device_id_t next_device_id);
} // namespace store

} // namespace data_model

struct fabric_manager
{
    fabric_core_handle_t core;
    nvs_handle_t nvs;
    data_model::device_set set;
    data_model::report *reports;
    fabric_manager_device_id_t next_device_id;
    fabric_manager_listener_id_t next_listener_id;
    SemaphoreHandle_t state_mutex;
    SemaphoreHandle_t listener_mutex;
    data_model::listener listeners[CONFIG_FABRIC_MANAGER_MAX_LISTENERS];
    data_model::listener_call *listener_calls;
};
