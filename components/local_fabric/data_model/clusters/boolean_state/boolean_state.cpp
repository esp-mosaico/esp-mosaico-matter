/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "boolean_state.h"
#include <app/data-model/Decode.h>

namespace data_model::clusters::boolean_state {
namespace {
constexpr uint32_t kClusterId = UINT32_C(0x0045);
struct state { bool reported; bool value; };
static_assert(sizeof(state) <= FABRIC_MANAGER_CLUSTER_STATE_MAX);
CHIP_ERROR decode_attribute(uint32_t attribute_id, chip::TLV::TLVReader &reader,
                            data_model::cluster_state *storage)
{
    if (attribute_id != UINT32_C(0x0000)) return CHIP_ERROR_NOT_FOUND;
    auto &current = *reinterpret_cast<state *>(storage->bytes);
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, current.value));
    current.reported = true;
    return CHIP_NO_ERROR;
}
esp_err_t encode_json(const data_model::cluster_state *storage, cJSON *properties,
                      cJSON *)
{
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    return json::add_property(properties, "state",
                        current.reported ? cJSON_CreateBool(current.value)
                                         : cJSON_CreateNull());
}
} // namespace
const data_model::cluster_adapter adapter = {
    .key = "boolean_state", .cluster_id = kClusterId,
    .decode_attribute = decode_attribute, .encode_json = encode_json};
} // namespace data_model::clusters::boolean_state
