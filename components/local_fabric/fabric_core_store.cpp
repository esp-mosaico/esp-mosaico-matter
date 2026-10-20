/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_core_private.h"

#include "esp_check.h"
#include "esp_random.h"

#include <lib/core/DataModelTypes.h>
#include <lib/core/NodeId.h>

#include <cstring>

namespace {

const char *const kTag = "fabric_core_store";
constexpr uint32_t kMagic = 0x4c464331; // LFC1
constexpr uint16_t kVersion = 1;
struct identity_blob {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint64_t controller_node_id;
    uint64_t fabric_id;
    uint64_t next_node_id;
};

esp_err_t fabric_core_store_identity(fabric_core_handle_t core)
{
    identity_blob blob = {
        .magic = kMagic,
        .version = kVersion,
        .reserved = 0,
        .controller_node_id = core->controller_node_id,
        .fabric_id = core->fabric_id,
        .next_node_id = core->next_node_id,
    };
    ESP_RETURN_ON_ERROR(nvs_set_blob(core->nvs, FABRIC_CORE_IDENTITY_KEY, &blob,
                                     sizeof(blob)), kTag, "store identity");
    return nvs_commit(core->nvs);
}

} // namespace

esp_err_t fabric_core_store_load(fabric_core_handle_t core)
{
    identity_blob blob = {};
    size_t size = sizeof(blob);
    esp_err_t error = nvs_get_blob(core->nvs, FABRIC_CORE_IDENTITY_KEY, &blob, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        do {
            esp_fill_random(&blob.controller_node_id, sizeof(blob.controller_node_id));
        } while (!chip::IsOperationalNodeId(blob.controller_node_id));
        do {
            esp_fill_random(&blob.fabric_id, sizeof(blob.fabric_id));
        } while (!chip::IsValidFabricId(blob.fabric_id));
        blob.magic = kMagic;
        blob.version = kVersion;
        blob.next_node_id = 1;
        core->controller_node_id = blob.controller_node_id;
        core->fabric_id = blob.fabric_id;
        core->next_node_id = blob.next_node_id;
        ESP_RETURN_ON_ERROR(fabric_core_store_identity(core), kTag, "create identity");
    } else {
        ESP_RETURN_ON_ERROR(error, kTag, "load identity");
        ESP_RETURN_ON_FALSE(size == sizeof(blob) && blob.magic == kMagic &&
                                blob.version == kVersion &&
                                chip::IsOperationalNodeId(blob.controller_node_id) &&
                                chip::IsValidFabricId(blob.fabric_id) &&
                                blob.next_node_id != 0,
                            ESP_ERR_INVALID_VERSION, kTag, "invalid identity");
        core->controller_node_id = blob.controller_node_id;
        core->fabric_id = blob.fabric_id;
        core->next_node_id = blob.next_node_id;
    }

    size = 0;
    error = nvs_get_blob(core->nvs, FABRIC_CORE_NODE_IDS_KEY, nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(error, kTag, "query nodes");
    ESP_RETURN_ON_FALSE(size % sizeof(uint64_t) == 0 &&
                            size / sizeof(uint64_t) <= core->config.max_nodes,
                        ESP_ERR_INVALID_SIZE, kTag, "invalid node list");
    core->node_count = size / sizeof(uint64_t);
    uint64_t *ids = static_cast<uint64_t *>(calloc(core->node_count, sizeof(uint64_t)));
    ESP_RETURN_ON_FALSE(ids || core->node_count == 0, ESP_ERR_NO_MEM, kTag,
                        "allocate node list");
    error = nvs_get_blob(core->nvs, FABRIC_CORE_NODE_IDS_KEY, ids, &size);
    if (error == ESP_OK) {
        for (size_t i = 0; i < core->node_count; ++i) {
            core->nodes[i].node_id = ids[i];
            core->nodes[i].subscription_state = FABRIC_CORE_SUBSCRIPTION_TERMINATED;
        }
    }
    free(ids);
    return error;
}

esp_err_t fabric_core_store_nodes(fabric_core_handle_t core)
{
    uint64_t *ids = static_cast<uint64_t *>(calloc(core->node_count, sizeof(uint64_t)));
    ESP_RETURN_ON_FALSE(ids || core->node_count == 0, ESP_ERR_NO_MEM, kTag,
                        "allocate node list");
    for (size_t i = 0; i < core->node_count; ++i) {
        ids[i] = core->nodes[i].node_id;
    }
    esp_err_t error = core->node_count
        ? nvs_set_blob(core->nvs, FABRIC_CORE_NODE_IDS_KEY, ids,
                       core->node_count * sizeof(uint64_t))
        : nvs_erase_key(core->nvs, FABRIC_CORE_NODE_IDS_KEY);
    free(ids);
    if (error == ESP_ERR_NVS_NOT_FOUND && core->node_count == 0) {
        error = ESP_OK;
    }
    if (error == ESP_OK) {
        error = fabric_core_store_identity(core);
    }
    return error;
}

esp_err_t fabric_core_store_erase(fabric_core_handle_t core)
{
    ESP_RETURN_ON_ERROR(nvs_erase_all(core->nvs), kTag, "erase core state");
    return nvs_commit(core->nvs);
}
