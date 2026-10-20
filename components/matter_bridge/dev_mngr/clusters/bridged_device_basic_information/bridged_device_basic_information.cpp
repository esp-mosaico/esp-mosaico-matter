/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "bridged_device_basic_information.h"

#include <cstring>

using namespace esp_matter;

namespace bridge::clusters::bridged_device_basic_information {

esp_err_t create(endpoint_t *endpoint, device_record &device)
{
    auto *basic = cluster::get(endpoint, kClusterId);
    if (!basic) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = cluster::destroy(basic);
    if (error != ESP_OK) {
        return error;
    }
    cluster::bridged_device_basic_information::config_t identity;
    identity.reachable = false;
    memcpy(identity.unique_id, device.definition.id, sizeof(identity.unique_id));
    basic = cluster::bridged_device_basic_information::create(
        endpoint, &identity, CLUSTER_FLAG_SERVER);
    if (!basic ||
        !cluster::bridged_device_basic_information::attribute::create_node_label(
            basic, device.definition.name, strlen(device.definition.name))) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t set_name(uint16_t endpoint_id, const char *name)
{
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    bridge_dev_mngr_value_t value{};
    value.type = BRIDGE_DEV_MNGR_VALUE_STRING;
    snprintf(value.text, sizeof(value.text), "%s", name);
    return generic_attribute_report(
        endpoint_id, {kClusterId, kNodeLabelAttributeId}, value);
}

esp_err_t set_reachable(uint16_t endpoint_id, bool reachable)
{
    bridge_dev_mngr_value_t value{};
    value.type = BRIDGE_DEV_MNGR_VALUE_BOOL;
    value.data.boolean = reachable;
    return generic_attribute_report(
        endpoint_id, {kClusterId, kReachableAttributeId}, value);
}

const cluster_adapter adapter = {
    .cluster_id = kClusterId,
    .read = generic_attribute_read,
    .report = generic_attribute_report,
};

} // namespace bridge::clusters::bridged_device_basic_information
