/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "illuminance_measurement.h"
#include <app/data-model/Decode.h>
#include <app/data-model/Nullable.h>
#include <cmath>

namespace data_model::clusters::illuminance_measurement {
namespace {
constexpr uint32_t kClusterId = UINT32_C(0x0400);
struct state { bool reported; bool is_null; uint16_t measured_value; };
static_assert(sizeof(state) <= FABRIC_MANAGER_CLUSTER_STATE_MAX);
CHIP_ERROR decode_attribute(uint32_t attribute_id, chip::TLV::TLVReader &reader,
                            data_model::cluster_state *storage)
{
    if (attribute_id != UINT32_C(0x0000)) return CHIP_ERROR_NOT_FOUND;
    chip::app::DataModel::Nullable<uint16_t> value;
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, value));
    auto &current = *reinterpret_cast<state *>(storage->bytes);
    current.reported = true;
    current.is_null = value.IsNull();
    current.measured_value = value.IsNull() ? 0 : value.Value();
    return CHIP_NO_ERROR;
}
esp_err_t encode_json(const data_model::cluster_state *storage, cJSON *properties,
                      cJSON *)
{
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    cJSON *value = cJSON_CreateNull();
    if (current.reported && !current.is_null) {
        const double lux = current.measured_value == 0
                               ? 0.0
                               : std::pow(10.0,
                                          (current.measured_value - 1.0) / 10000.0);
        cJSON_Delete(value);
        value = cJSON_CreateNumber(lux);
    }
    return json::add_property(properties, "illuminance_lux", value, "lux");
}
} // namespace
const data_model::cluster_adapter adapter = {
    .key = "illuminance_measurement", .cluster_id = kClusterId,
    .decode_attribute = decode_attribute, .encode_json = encode_json};
} // namespace data_model::clusters::illuminance_measurement
