/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "occupancy_sensing.h"

#include "esp_matter_data_model_provider.h"
#include <app/clusters/occupancy-sensor-server/OccupancySensingCluster.h>
#include <clusters/OccupancySensing/ClusterId.h>

namespace bridge::clusters::occupancy_sensing {
namespace {

chip::app::Clusters::OccupancySensingCluster *server(uint16_t endpoint_id)
{
    auto *cluster = esp_matter::data_model::provider::get_instance()
                        .registry()
                        .Get(chip::app::ConcreteClusterPath(
                            endpoint_id,
                            chip::app::Clusters::OccupancySensing::Id));
    return static_cast<chip::app::Clusters::OccupancySensingCluster *>(cluster);
}

esp_err_t read(uint16_t endpoint_id,
               const bridge_dev_mngr_attribute_path_t &path,
               bridge_dev_mngr_value_t &out_value)
{
    if (path.attribute_id != kOccupancyAttributeId) {
        return generic_attribute_read(endpoint_id, path, out_value);
    }
    auto *cluster = server(endpoint_id);
    if (!cluster) {
        return ESP_ERR_NOT_FOUND;
    }
    out_value = {};
    out_value.type = BRIDGE_DEV_MNGR_VALUE_BOOL;
    out_value.data.boolean = cluster->IsOccupied();
    return ESP_OK;
}

esp_err_t report(uint16_t endpoint_id,
                 const bridge_dev_mngr_attribute_path_t &path,
                 const bridge_dev_mngr_value_t &value)
{
    if (path.attribute_id != kOccupancyAttributeId) {
        return generic_attribute_report(endpoint_id, path, value);
    }
    if (value.type != BRIDGE_DEV_MNGR_VALUE_BOOL || value.truncated) {
        return ESP_ERR_INVALID_ARG;
    }
    auto *cluster = server(endpoint_id);
    if (!cluster) {
        return ESP_ERR_NOT_FOUND;
    }
    cluster->SetOccupancy(value.data.boolean);
    return ESP_OK;
}

} // namespace

const cluster_adapter adapter = {
    .cluster_id = kClusterId,
    .read = read,
    .report = report,
};

} // namespace bridge::clusters::occupancy_sensing
