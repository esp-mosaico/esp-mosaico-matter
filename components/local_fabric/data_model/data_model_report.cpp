/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_manager_private.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app/data-model/Decode.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

const char *const kTag = "fabric_manager_report";
constexpr uint32_t kDescriptorClusterId = UINT32_C(0x001d);
constexpr uint32_t kDeviceTypeListAttributeId = UINT32_C(0x0000);
constexpr uint32_t kServerListAttributeId = UINT32_C(0x0001);
constexpr uint32_t kPartsListAttributeId = UINT32_C(0x0003);
constexpr uint32_t kTagListAttributeId = UINT32_C(0x0004);

data_model::reported_endpoint *get_endpoint(data_model::report &transaction,
                                      uint16_t endpoint_id)
{
    for (size_t index = 0; index < transaction.endpoint_count; ++index) {
        if (transaction.endpoints[index].endpoint_id == endpoint_id) {
            return &transaction.endpoints[index];
        }
    }
    if (transaction.endpoint_count >= CONFIG_FABRIC_MANAGER_MAX_ENDPOINTS) {
        transaction.overflowed = true;
        return nullptr;
    }
    auto &endpoint = transaction.endpoints[transaction.endpoint_count++];
    endpoint.endpoint_id = endpoint_id;
    return &endpoint;
}

void remove_device_types(data_model::report &transaction, uint16_t endpoint_id)
{
    size_t output = 0;
    for (size_t index = 0; index < transaction.device_type_count; ++index) {
        if (transaction.device_types[index].endpoint_id != endpoint_id) {
            transaction.device_types[output++] =
                transaction.device_types[index];
        }
    }
    transaction.device_type_count = output;
}

bool record_device_type(data_model::report &transaction, uint16_t endpoint_id,
                        uint32_t device_type)
{
    size_t endpoint_count = 0;
    for (size_t index = 0; index < transaction.device_type_count; ++index) {
        const auto &record = transaction.device_types[index];
        if (record.endpoint_id == endpoint_id)
            ++endpoint_count;
        if (record.endpoint_id == endpoint_id &&
            record.device_type == device_type) {
            return true;
        }
    }
    constexpr size_t kCapacity = CONFIG_FABRIC_MANAGER_MAX_ENDPOINTS *
                                 CONFIG_FABRIC_MANAGER_MAX_DEVICE_TYPES_PER_ENDPOINT;
    if (endpoint_count >= CONFIG_FABRIC_MANAGER_MAX_DEVICE_TYPES_PER_ENDPOINT ||
        transaction.device_type_count >= kCapacity) {
        return false;
    }
    transaction.device_types[transaction.device_type_count++] = {
        .endpoint_id = endpoint_id,
        .device_type = device_type,
    };
    return true;
}

CHIP_ERROR decode_device_type_list(chip::TLV::TLVReader &reader,
                                   data_model::report &transaction,
                                   uint16_t endpoint_id)
{
    using DeviceTypes = chip::app::Clusters::Descriptor::Attributes::
        DeviceTypeList::TypeInfo::DecodableType;
    DeviceTypes values;
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, values));
    remove_device_types(transaction, endpoint_id);
    auto iterator = values.begin();
    while (iterator.Next()) {
        VerifyOrReturnError(record_device_type(transaction, endpoint_id,
                                               iterator.GetValue().deviceType),
                            CHIP_ERROR_BUFFER_TOO_SMALL);
    }
    return iterator.GetStatus();
}

void remove_relationships(data_model::report &transaction,
                          uint16_t parent_endpoint_id)
{
    size_t output = 0;
    for (size_t index = 0; index < transaction.relationship_count; ++index) {
        if (transaction.relationships[index].parent_endpoint_id !=
            parent_endpoint_id) {
            transaction.relationships[output++] =
                transaction.relationships[index];
        }
    }
    transaction.relationship_count = output;
}

bool record_relationship(data_model::report &transaction, uint16_t parent_endpoint_id,
                         uint16_t child_endpoint_id)
{
    if (parent_endpoint_id == child_endpoint_id)
        return false;
    for (size_t index = 0; index < transaction.relationship_count; ++index) {
        const auto &relationship = transaction.relationships[index];
        if (relationship.parent_endpoint_id == parent_endpoint_id &&
            relationship.child_endpoint_id == child_endpoint_id) {
            return true;
        }
    }
    if (transaction.relationship_count >=
        CONFIG_FABRIC_MANAGER_MAX_REPORT_RELATIONSHIPS) {
        return false;
    }
    transaction.relationships[transaction.relationship_count++] = {
        .parent_endpoint_id = parent_endpoint_id,
        .child_endpoint_id = child_endpoint_id,
    };
    return true;
}

CHIP_ERROR decode_parts_list(chip::TLV::TLVReader &reader,
                             data_model::report &transaction,
                             uint16_t parent_endpoint_id)
{
    using Parts = chip::app::Clusters::Descriptor::Attributes::PartsList::
        TypeInfo::DecodableType;
    Parts values;
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, values));
    remove_relationships(transaction, parent_endpoint_id);
    auto iterator = values.begin();
    while (iterator.Next()) {
        const uint16_t child = iterator.GetValue();
        VerifyOrReturnError(
            record_relationship(transaction, parent_endpoint_id, child),
            CHIP_ERROR_BUFFER_TOO_SMALL);
        VerifyOrReturnError(get_endpoint(transaction, child) != nullptr,
                            CHIP_ERROR_BUFFER_TOO_SMALL);
    }
    return iterator.GetStatus();
}

size_t adapter_index(const data_model::cluster_adapter *adapter)
{
    for (size_t index = 0; index < data_model::cluster_count(); ++index) {
        if (data_model::cluster_at(index) == adapter)
            return index;
    }
    return data_model::cluster_count();
}

CHIP_ERROR decode_server_list(chip::TLV::TLVReader &reader,
                              data_model::report &transaction,
                              data_model::reported_endpoint &endpoint)
{
    using Servers = chip::app::Clusters::Descriptor::Attributes::ServerList::
        TypeInfo::DecodableType;
    Servers values;
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, values));
    endpoint.cluster_mask = 0;
    endpoint.server_list_reported = true;
    auto iterator = values.begin();
    while (iterator.Next()) {
        const auto *adapter = data_model::find_cluster(iterator.GetValue());
        const size_t index = adapter_index(adapter);
        if (index < 32U)
            endpoint.cluster_mask |= UINT32_C(1) << index;
    }
    return iterator.GetStatus();
}

CHIP_ERROR decode_tag_list(chip::TLV::TLVReader &reader,
                           data_model::reported_endpoint &endpoint)
{
    using Tags = chip::app::Clusters::Descriptor::Attributes::TagList::
        TypeInfo::DecodableType;
    Tags values;
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, values));

    char label[FABRIC_MANAGER_ENDPOINT_NAME_MAX] = {};
    const char *location = nullptr;
    const char *position = nullptr;
    size_t count = 0;
    auto iterator = values.begin();
    while (count < CONFIG_FABRIC_MANAGER_MAX_TAGS_PER_ENDPOINT && iterator.Next()) {
        ++count;
        const auto &tag = iterator.GetValue();
        if (tag.label.HasValue() && !tag.label.Value().IsNull() && !label[0]) {
            const auto text = tag.label.Value().Value();
            const char *first = text.data();
            size_t length = text.size();
            while (length && *first == ' ') {
                ++first;
                --length;
            }
            while (length && first[length - 1] == ' ')
                --length;
            if (length && length < sizeof(label)) {
                bool printable = true;
                for (size_t index = 0; index < length; ++index) {
                    if (static_cast<unsigned char>(first[index]) < 0x20 ||
                        first[index] == 0x7f)
                        printable = false;
                }
                if (printable) {
                    memcpy(label, first, length);
                    label[length] = '\0';
                }
            }
        }
        if (!tag.mfgCode.IsNull())
            continue;
        // Matter Standard Namespaces: Common Location 0x06, Position 0x08.
        if (tag.namespaceID == 0x06 && !location) {
            static const char *const kLocation[] = {
                "Indoor", "Outdoor", "Inside", "Outside",
            };
            if (tag.tag < sizeof(kLocation) / sizeof(kLocation[0]))
                location = kLocation[tag.tag];
        } else if (tag.namespaceID == 0x08 && !position) {
            static const char *const kPosition[] = {
                "Left", "Right", "Top", "Bottom", "Middle",
            };
            if (tag.tag < sizeof(kPosition) / sizeof(kPosition[0]))
                position = kPosition[tag.tag];
        }
    }
    if (count < CONFIG_FABRIC_MANAGER_MAX_TAGS_PER_ENDPOINT)
        ReturnErrorOnFailure(iterator.GetStatus());
    if (label[0]) {
        snprintf(endpoint.tag_name, sizeof(endpoint.tag_name), "%s", label);
    } else if (location && position) {
        snprintf(endpoint.tag_name, sizeof(endpoint.tag_name), "%s %s",
                 location, position);
    } else {
        snprintf(endpoint.tag_name, sizeof(endpoint.tag_name), "%s",
                 location ? location : position ? position : "");
    }
    endpoint.tag_list_reported = true;
    return CHIP_NO_ERROR;
}

CHIP_ERROR record_descriptor(chip::TLV::TLVReader &reader,
                             data_model::report &transaction, uint16_t endpoint_id,
                             uint32_t attribute_id)
{
    auto *endpoint = get_endpoint(transaction, endpoint_id);
    VerifyOrReturnError(endpoint != nullptr, CHIP_ERROR_BUFFER_TOO_SMALL);
    switch (attribute_id) {
    case kDeviceTypeListAttributeId:
        return decode_device_type_list(reader, transaction, endpoint_id);
    case kServerListAttributeId:
        return decode_server_list(reader, transaction, *endpoint);
    case kPartsListAttributeId:
        return decode_parts_list(reader, transaction, endpoint_id);
    case kTagListAttributeId: {
        const CHIP_ERROR error = decode_tag_list(reader, *endpoint);
        if (error != CHIP_NO_ERROR) {
            ESP_LOGW(kTag, "ignore invalid TagList on endpoint %u: %s",
                     endpoint_id, error.AsString());
        }
        return CHIP_NO_ERROR;
    }
    default:
        return CHIP_NO_ERROR;
    }
}

bool copy_committed_state(fabric_manager_handle_t manager, uint64_t node_id,
                          uint16_t endpoint_id,
                          const data_model::cluster_adapter *adapter,
                          data_model::cluster_state *out_state)
{
    data_model::lock_guard lock(manager->state_mutex);
    for (size_t endpoint_index = 0; endpoint_index < manager->set.endpoint_count;
         ++endpoint_index) {
        const auto &endpoint = manager->set.endpoints[endpoint_index];
        const auto &device = manager->set.devices[endpoint.device_index];
        if (device.node_id != node_id || endpoint.endpoint_id != endpoint_id) {
            continue;
        }
        for (size_t offset = 0; offset < endpoint.cluster_count; ++offset) {
            const auto &cluster =
                manager->set.clusters[endpoint.first_cluster + offset];
            if (cluster.adapter == adapter) {
                *out_state = cluster.state;
                return true;
            }
        }
    }
    return false;
}

data_model::reported_cluster *get_report_cluster(fabric_manager_handle_t manager,
                                           data_model::report &transaction,
                                           uint16_t endpoint_id, size_t index,
                                           const data_model::cluster_adapter *adapter)
{
    size_t endpoint_cluster_count = 0;
    for (size_t cluster_index = 0; cluster_index < transaction.cluster_count;
         ++cluster_index) {
        auto &cluster = transaction.clusters[cluster_index];
        if (cluster.endpoint_id == endpoint_id)
            ++endpoint_cluster_count;
        if (cluster.endpoint_id == endpoint_id &&
            cluster.adapter_index == index) {
            return &cluster;
        }
    }
    if (endpoint_cluster_count >= CONFIG_FABRIC_MANAGER_MAX_CLUSTERS_PER_ENDPOINT ||
        transaction.cluster_count >= FABRIC_MANAGER_REPORT_CLUSTERS_MAX) {
        transaction.overflowed = true;
        return nullptr;
    }
    auto &cluster = transaction.clusters[transaction.cluster_count++];
    cluster.endpoint_id = endpoint_id;
    cluster.adapter_index = static_cast<uint8_t>(index);
    copy_committed_state(manager, transaction.node_id, endpoint_id, adapter,
                         &cluster.state);
    return &cluster;
}

CHIP_ERROR record_cluster_attribute(fabric_manager_handle_t manager,
                                    chip::TLV::TLVReader &reader,
                                    data_model::report &transaction,
                                    uint16_t endpoint_id, uint32_t cluster_id,
                                    uint32_t attribute_id)
{
    const auto *adapter = data_model::find_cluster(cluster_id);
    if (!adapter)
        return CHIP_NO_ERROR;
    const size_t index = adapter_index(adapter);
    VerifyOrReturnError(index < data_model::cluster_count(), CHIP_ERROR_INTERNAL);
    auto *endpoint = get_endpoint(transaction, endpoint_id);
    VerifyOrReturnError(endpoint != nullptr, CHIP_ERROR_BUFFER_TOO_SMALL);
    if (!endpoint->server_list_reported && index < 32U) {
        endpoint->cluster_mask |= UINT32_C(1) << index;
    }
    auto *cluster =
        get_report_cluster(manager, transaction, endpoint_id, index, adapter);
    VerifyOrReturnError(cluster != nullptr, CHIP_ERROR_BUFFER_TOO_SMALL);
    data_model::cluster_state decoded = cluster->state;
    CHIP_ERROR error =
        adapter->decode_attribute(attribute_id, reader, &decoded);
    if (error == CHIP_ERROR_NOT_FOUND)
        return CHIP_NO_ERROR;
    ReturnErrorOnFailure(error);
    cluster->state = decoded;
    cluster->touched = true;
    return CHIP_NO_ERROR;
}

void release_changes(data_model::property_change *changes, size_t change_count)
{
    for (size_t index = 0; index < change_count; ++index) {
        cJSON_Delete(changes[index].previous_value);
        cJSON_Delete(changes[index].value);
    }
    free(changes);
}

data_model::report *find_report(fabric_manager_handle_t manager, uint64_t node_id)
{
    for (auto *transaction = manager->reports; transaction;
         transaction = transaction->next) {
        if (transaction->node_id == node_id)
            return transaction;
    }
    return nullptr;
}

data_model::report *take_report(fabric_manager_handle_t manager, uint64_t node_id)
{
    auto **link = &manager->reports;
    while (*link) {
        if ((*link)->node_id == node_id) {
            auto *transaction = *link;
            *link = transaction->next;
            transaction->next = nullptr;
            return transaction;
        }
        link = &(*link)->next;
    }
    return nullptr;
}

esp_err_t commit_snapshot(fabric_manager_handle_t manager,
                          const data_model::report &transaction)
{
    data_model::device_set replacement = {};
    esp_err_t error = ESP_OK;
    {
        data_model::lock_guard lock(manager->state_mutex);
        error = data_model::model::replace_node(manager, transaction, &replacement);
        if (error == ESP_OK) {
            error = data_model::store::save(manager, replacement.devices,
                                      replacement.device_count,
                                      manager->next_device_id);
        }
        if (error == ESP_OK) {
            data_model::device_set previous = manager->set;
            manager->set = replacement;
            replacement = {};
            data_model::model::free(&previous);
        }
    }
    data_model::model::free(&replacement);
    if (error == ESP_OK)
        data_model::dispatch_device_set_changed(manager);
    return error;
}

esp_err_t apply_change_report(fabric_manager_handle_t manager,
                              const data_model::report &transaction)
{
    data_model::property_change *changes = nullptr;
    size_t change_count = 0;
    bool structure_changed = false;
    esp_err_t error = ESP_OK;
    {
        data_model::lock_guard lock(manager->state_mutex);
        error = data_model::model::apply(manager, transaction, &changes, &change_count,
                                   &structure_changed);
    }
    if (error == ESP_OK && change_count != 0)
        data_model::dispatch_property_changes(manager, changes, change_count);
    if (error == ESP_OK && structure_changed)
        data_model::dispatch_device_set_changed(manager);
    release_changes(changes, change_count);
    return error;
}

} // namespace

void data_model::report_begin(fabric_core_handle_t, uint64_t node_id, void *user_ctx)
{
    auto manager = static_cast<fabric_manager_handle_t>(user_ctx);
    if (!manager || find_report(manager, node_id))
        return;
    auto *transaction = static_cast<data_model::report *>(heap_caps_calloc(
        1, sizeof(data_model::report), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!transaction) {
        transaction =
            static_cast<data_model::report *>(calloc(1, sizeof(*transaction)));
    }
    if (transaction) {
        transaction->node_id = node_id;
        transaction->next = manager->reports;
        manager->reports = transaction;
    }
}

void data_model::attribute_reported(fabric_core_handle_t,
                              const fabric_core_private_attribute_report *report,
                              void *user_ctx)
{
    auto manager = static_cast<fabric_manager_handle_t>(user_ctx);
    if (!manager || !report || !report->data || report->error != CHIP_NO_ERROR)
        return;
    auto *transaction =
        find_report(manager, report->path.endpoint.node_id);
    if (!transaction || transaction->overflowed ||
        transaction->descriptor_invalid) {
        return;
    }
    chip::TLV::TLVReader reader = *report->data;
    CHIP_ERROR error = CHIP_NO_ERROR;
    if (report->path.cluster_id == kDescriptorClusterId) {
        error = record_descriptor(reader, *transaction,
                                  report->path.endpoint.endpoint_id,
                                  report->path.attribute_id);
        if (error != CHIP_NO_ERROR)
            transaction->descriptor_invalid = true;
    } else {
        error = record_cluster_attribute(
            manager, reader, *transaction, report->path.endpoint.endpoint_id,
            report->path.cluster_id, report->path.attribute_id);
        if (error == CHIP_ERROR_BUFFER_TOO_SMALL) {
            transaction->overflowed = true;
        }
    }
    if (error != CHIP_NO_ERROR) {
        ESP_LOGW(kTag,
                 "ignore invalid report path 0x%04" PRIx32 "/0x%04" PRIx32
                 ": %s",
                 report->path.cluster_id, report->path.attribute_id,
                 error.AsString());
    }
}

void data_model::report_end(fabric_core_handle_t, uint64_t node_id, esp_err_t error,
                      bool snapshot_complete, void *user_ctx)
{
    auto manager = static_cast<fabric_manager_handle_t>(user_ctx);
    if (!manager)
        return;
    auto *transaction = take_report(manager, node_id);
    if (!transaction)
        return;

    if (error != ESP_OK || transaction->overflowed ||
        transaction->descriptor_invalid) {
        ESP_LOGW(kTag, "discard report for node 0x%016" PRIx64,
                 transaction->node_id);
        free(transaction);
        return;
    }

    error = snapshot_complete ? commit_snapshot(manager, *transaction)
                              : apply_change_report(manager, *transaction);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "failed to %s: %s",
                 snapshot_complete ? "commit snapshot" : "apply report",
                 esp_err_to_name(error));
    }
    free(transaction);
}
