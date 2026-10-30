/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "illuminance_measurement.h"

#include "esp_matter_data_model_provider.h"
#include <app/clusters/illuminance-measurement-server/IlluminanceMeasurementCluster.h>
#include <clusters/IlluminanceMeasurement/ClusterId.h>

namespace bridge::clusters::illuminance_measurement {
namespace {

chip::app::Clusters::IlluminanceMeasurementCluster *server(uint16_t endpoint_id)
{
    auto *cluster = esp_matter::data_model::provider::get_instance()
                        .registry()
                        .Get(chip::app::ConcreteClusterPath(endpoint_id, kClusterId));
    return static_cast<chip::app::Clusters::IlluminanceMeasurementCluster *>(cluster);
}

esp_err_t read(uint16_t endpoint_id, const bridge_dev_mngr_attribute_path_t &path,
               bridge_dev_mngr_value_t &out_value)
{
    if (path.attribute_id > 2) return generic_attribute_read(endpoint_id, path, out_value);
    auto *cluster = server(endpoint_id);
    if (!cluster) return ESP_ERR_NOT_FOUND;
    const auto value = path.attribute_id == 0 ? cluster->GetMeasuredValue()
        : path.attribute_id == 1 ? cluster->GetMinMeasuredValue() : cluster->GetMaxMeasuredValue();
    out_value = {};
    out_value.type = value.IsNull() ? BRIDGE_DEV_MNGR_VALUE_NULL : BRIDGE_DEV_MNGR_VALUE_UNSIGNED;
    if (!value.IsNull()) out_value.data.unsigned_value = value.Value();
    return ESP_OK;
}

esp_err_t report(uint16_t endpoint_id, const bridge_dev_mngr_attribute_path_t &path,
                 const bridge_dev_mngr_value_t &value)
{
    // Limits belong to the endpoint configuration; only MeasuredValue is reportable.
    if (path.attribute_id == 1 || path.attribute_id == 2) return ESP_ERR_NOT_SUPPORTED;
    if (path.attribute_id != 0) return generic_attribute_report(endpoint_id, path, value);
    if (value.truncated || (value.type != BRIDGE_DEV_MNGR_VALUE_NULL &&
        (value.type != BRIDGE_DEV_MNGR_VALUE_UNSIGNED || value.data.unsigned_value > 65534))) {
        return ESP_ERR_INVALID_ARG;
    }
    auto *cluster = server(endpoint_id);
    if (!cluster) return ESP_ERR_NOT_FOUND;
    chip::app::DataModel::Nullable<uint16_t> measured;
    if (value.type != BRIDGE_DEV_MNGR_VALUE_NULL) {
        measured.SetNonNull(static_cast<uint16_t>(value.data.unsigned_value));
    }
    return cluster->SetMeasuredValue(measured) == CHIP_NO_ERROR ? ESP_OK : ESP_ERR_INVALID_ARG;
}

} // namespace

const cluster_adapter adapter = {
    .cluster_id = kClusterId,
    .read = read,
    .report = report,
};

} // namespace bridge::clusters::illuminance_measurement
