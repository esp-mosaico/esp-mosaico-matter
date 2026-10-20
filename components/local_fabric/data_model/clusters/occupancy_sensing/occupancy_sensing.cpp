/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "occupancy_sensing.h"
#include <app/data-model/Decode.h>
#include <clusters/OccupancySensing/Attributes.h>

namespace data_model::clusters::occupancy_sensing {
namespace {
constexpr uint32_t kClusterId = UINT32_C(0x0406);
struct state { bool reported; bool occupied; };
static_assert(sizeof(state) <= FABRIC_MANAGER_CLUSTER_STATE_MAX);
CHIP_ERROR decode_attribute(uint32_t attribute_id, chip::TLV::TLVReader &reader,
                            data_model::cluster_state *storage)
{
    if (attribute_id != UINT32_C(0x0000)) return CHIP_ERROR_NOT_FOUND;
    using namespace chip::app::Clusters::OccupancySensing;
    Attributes::Occupancy::TypeInfo::DecodableType value;
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, value));
    VerifyOrReturnError((value.Raw() & ~UINT8_C(0x01)) == 0,
                        CHIP_ERROR_INVALID_ARGUMENT);
    auto &current = *reinterpret_cast<state *>(storage->bytes);
    current.reported = true;
    current.occupied = value.Has(OccupancyBitmap::kOccupied);
    return CHIP_NO_ERROR;
}
esp_err_t encode_json(const data_model::cluster_state *storage, cJSON *properties,
                      cJSON *)
{
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    return json::add_property(properties, "occupied",
                        current.reported ? cJSON_CreateBool(current.occupied)
                                         : cJSON_CreateNull());
}
} // namespace
const data_model::cluster_adapter adapter = {
    .key = "occupancy_sensing", .cluster_id = kClusterId,
    .decode_attribute = decode_attribute, .encode_json = encode_json};
} // namespace data_model::clusters::occupancy_sensing
