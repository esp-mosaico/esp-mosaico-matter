/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "temperature_measurement.h"
#include <app/data-model/Decode.h>
#include <app/data-model/Nullable.h>

namespace data_model::clusters::temperature_measurement {
namespace {
constexpr uint32_t kClusterId = UINT32_C(0x0402);
struct state { bool reported; bool is_null; int16_t hundredths_celsius; };
static_assert(sizeof(state) <= FABRIC_MANAGER_CLUSTER_STATE_MAX);
CHIP_ERROR decode_attribute(uint32_t attribute_id, chip::TLV::TLVReader &reader,
                            data_model::cluster_state *storage)
{
    if (attribute_id != UINT32_C(0x0000)) return CHIP_ERROR_NOT_FOUND;
    chip::app::DataModel::Nullable<int16_t> value;
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, value));
    VerifyOrReturnError(value.IsNull() || value.Value() >= -27315,
                        CHIP_ERROR_INVALID_ARGUMENT);
    auto &current = *reinterpret_cast<state *>(storage->bytes);
    current.reported = true;
    current.is_null = value.IsNull();
    current.hundredths_celsius = value.IsNull() ? 0 : value.Value();
    return CHIP_NO_ERROR;
}
esp_err_t encode_json(const data_model::cluster_state *storage, cJSON *properties,
                      cJSON *)
{
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    cJSON *value = current.reported && !current.is_null
                       ? cJSON_CreateNumber(current.hundredths_celsius / 100.0)
                       : cJSON_CreateNull();
    return json::add_property(properties, "temperature_celsius", value, "celsius");
}
} // namespace
const data_model::cluster_adapter adapter = {
    .key = "temperature_measurement", .cluster_id = kClusterId,
    .decode_attribute = decode_attribute, .encode_json = encode_json};
} // namespace data_model::clusters::temperature_measurement
