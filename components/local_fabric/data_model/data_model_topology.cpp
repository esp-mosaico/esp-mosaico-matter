/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_manager_private.h"

#include "esp_check.h"

#include <cstdio>
#include <cstring>

namespace
{

const char *const kTag = "fabric_manager_topology";
constexpr uint32_t kRootNodeDeviceType = UINT32_C(0x0016);
constexpr uint32_t kAggregatorDeviceType = UINT32_C(0x000e);

bool has_device_type(const data_model::report &report,
                     uint16_t endpoint_id, uint32_t device_type)
{
    for (size_t index = 0; index < report.device_type_count; ++index) {
        const auto &record = report.device_types[index];
        if (record.endpoint_id == endpoint_id &&
            record.device_type == device_type) {
            return true;
        }
    }
    return false;
}

bool is_container(const data_model::report &report,
                  uint16_t endpoint_id)
{
    return endpoint_id == 0 ||
           has_device_type(report, endpoint_id, kRootNodeDeviceType) ||
           has_device_type(report, endpoint_id, kAggregatorDeviceType);
}

bool lists_endpoint(const data_model::report &report, uint16_t parent_endpoint_id,
                    uint16_t child_endpoint_id)
{
    for (size_t index = 0; index < report.relationship_count; ++index) {
        const auto &relationship = report.relationships[index];
        if (relationship.parent_endpoint_id == parent_endpoint_id &&
            relationship.child_endpoint_id == child_endpoint_id) {
            return true;
        }
    }
    return false;
}

bool has_closer_parent(const data_model::report &report, uint16_t endpoint_id,
                       uint16_t candidate)
{
    for (size_t index = 0; index < report.relationship_count; ++index) {
        const auto &other = report.relationships[index];
        if (other.child_endpoint_id == endpoint_id &&
            other.parent_endpoint_id != 0 &&
            other.parent_endpoint_id != candidate &&
            lists_endpoint(report, candidate, other.parent_endpoint_id)) {
            return true;
        }
    }
    return false;
}

bool find_parent(const data_model::report &report,
                 uint16_t endpoint_id, uint16_t *out_parent,
                 bool *out_ambiguous)
{
    bool has_candidate = false;
    bool found = false;
    uint16_t parent = 0;
    *out_ambiguous = false;
    for (size_t index = 0; index < report.relationship_count; ++index) {
        const auto &relationship = report.relationships[index];
        const uint16_t candidate = relationship.parent_endpoint_id;
        if (relationship.child_endpoint_id != endpoint_id || candidate == 0) {
            continue;
        }
        has_candidate = true;
        if (has_closer_parent(report, endpoint_id, candidate))
            continue;
        if (!found) {
            parent = candidate;
            found = true;
        } else if (parent != candidate) {
            *out_ambiguous = true;
            return false;
        }
    }
    if (!found && has_candidate)
        *out_ambiguous = true;
    if (found)
        *out_parent = parent;
    return found;
}

bool resolve_root(const data_model::report &report,
                  uint16_t endpoint_id, uint16_t *out_root)
{
    uint16_t current = endpoint_id;
    for (size_t depth = 0; depth <= report.endpoint_count; ++depth) {
        uint16_t parent = 0;
        bool ambiguous = false;
        if (!find_parent(report, current, &parent, &ambiguous)) {
            if (ambiguous) return false;
            *out_root = current;
            return true;
        }
        if (has_device_type(report, parent, kAggregatorDeviceType)) {
            *out_root = current;
            return true;
        }
        if (parent == endpoint_id) return false;
        current = parent;
    }
    return false;
}

int type_priority(const char *type)
{
    if (!strcmp(type, "rvc")) return 90;
    if (!strcmp(type, "thermostat")) return 80;
    if (!strcmp(type, "fan")) return 70;
    if (!strcmp(type, "light")) return 60;
    if (!strcmp(type, "plug")) return 50;
    if (!strcmp(type, "switch")) return 40;
    if (!strcmp(type, "sensor")) return 30;
    return 0;
}

bool has_any_device_type(const data_model::report &report,
                         uint16_t endpoint_id, const uint32_t *types,
                         size_t type_count)
{
    for (size_t index = 0; index < type_count; ++index) {
        if (has_device_type(report, endpoint_id, types[index])) return true;
    }
    return false;
}

const char *type_for_endpoint(const data_model::report &report,
                              uint16_t endpoint_id)
{
    if (has_device_type(report, endpoint_id, UINT32_C(0x0074))) return "rvc";
    if (has_device_type(report, endpoint_id, UINT32_C(0x0301))) {
        return "thermostat";
    }
    if (has_device_type(report, endpoint_id, UINT32_C(0x002b))) return "fan";

    constexpr uint32_t kLightTypes[] = {
        UINT32_C(0x0100), UINT32_C(0x0101), UINT32_C(0x010c),
        UINT32_C(0x010d)};
    if (has_any_device_type(report, endpoint_id, kLightTypes,
                            sizeof(kLightTypes) / sizeof(kLightTypes[0]))) {
        return "light";
    }
    constexpr uint32_t kPlugTypes[] = {UINT32_C(0x010a), UINT32_C(0x010b)};
    if (has_any_device_type(report, endpoint_id, kPlugTypes,
                            sizeof(kPlugTypes) / sizeof(kPlugTypes[0]))) {
        return "plug";
    }
    constexpr uint32_t kSwitchTypes[] = {
        UINT32_C(0x000f), UINT32_C(0x0103), UINT32_C(0x0104),
        UINT32_C(0x0105), UINT32_C(0x010f), UINT32_C(0x0110)};
    if (has_any_device_type(report, endpoint_id, kSwitchTypes,
                            sizeof(kSwitchTypes) / sizeof(kSwitchTypes[0]))) {
        return "switch";
    }
    constexpr uint32_t kSensorTypes[] = {
        UINT32_C(0x0011), UINT32_C(0x0015), UINT32_C(0x0106),
        UINT32_C(0x0107), UINT32_C(0x0302), UINT32_C(0x0307)};
    if (has_any_device_type(report, endpoint_id, kSensorTypes,
                            sizeof(kSensorTypes) / sizeof(kSensorTypes[0]))) {
        return "sensor";
    }
    return "generic";
}

void update_type(data_model::group &group, const char *candidate)
{
    if (type_priority(candidate) > type_priority(group.type)) {
        snprintf(group.type, sizeof(group.type), "%s", candidate);
    }
}

} // namespace

esp_err_t data_model::topology::resolve(
    const data_model::report &report,
    data_model::group *groups, size_t *out_group_count,
    uint16_t *endpoint_roots)
{
    ESP_RETURN_ON_FALSE(groups && out_group_count && endpoint_roots,
                        ESP_ERR_INVALID_ARG, kTag, "invalid topology output");
    size_t group_count = 0;
    for (size_t endpoint_index = 0; endpoint_index < report.endpoint_count;
         ++endpoint_index) {
        const auto &endpoint = report.endpoints[endpoint_index];
        endpoint_roots[endpoint_index] = 0;
        if (endpoint.endpoint_id == 0 || endpoint.cluster_mask == 0 ||
            is_container(report, endpoint.endpoint_id)) {
            continue;
        }
        uint16_t root = 0;
        if (!resolve_root(report, endpoint.endpoint_id, &root) ||
            is_container(report, root)) {
            continue;
        }
        endpoint_roots[endpoint_index] = root;

        size_t group_index = 0;
        while (group_index < group_count &&
               groups[group_index].root_endpoint_id != root) {
            ++group_index;
        }
        if (group_index == group_count) {
            ESP_RETURN_ON_FALSE(group_count < CONFIG_FABRIC_MANAGER_MAX_DEVICES,
                                ESP_ERR_NO_MEM, kTag,
                                "resolved device limit exceeded");
            groups[group_count].root_endpoint_id = root;
            snprintf(groups[group_count].type,
                     sizeof(groups[group_count].type), "generic");
            group_index = group_count++;
            update_type(groups[group_index], type_for_endpoint(report, root));
        }
        update_type(groups[group_index],
                    type_for_endpoint(report, endpoint.endpoint_id));
    }
    *out_group_count = group_count;
    return ESP_OK;
}
