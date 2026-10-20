/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "on_off.h"


#include "esp_check.h"

#include <app/data-model/Decode.h>

#include <cstring>

namespace data_model::clusters::on_off
{
namespace
{

constexpr uint32_t kClusterId = UINT32_C(0x0006);
constexpr uint32_t kOnOffAttributeId = UINT32_C(0x0000);

struct state {
    bool reported;
    bool on;
};
static_assert(sizeof(state) <= FABRIC_MANAGER_CLUSTER_STATE_MAX);

CHIP_ERROR decode_attribute(uint32_t attribute_id,
                            chip::TLV::TLVReader &reader,
                            data_model::cluster_state *storage)
{
    if (attribute_id != kOnOffAttributeId) return CHIP_ERROR_NOT_FOUND;
    auto &current = *reinterpret_cast<state *>(storage->bytes);
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, current.on));
    current.reported = true;
    return CHIP_NO_ERROR;
}

esp_err_t encode_json(const data_model::cluster_state *storage, cJSON *properties,
                      cJSON *actions)
{
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    ESP_RETURN_ON_ERROR(
        json::add_property(properties, "on",
                     current.reported ? cJSON_CreateBool(current.on)
                                      : cJSON_CreateNull()),
        "on_off", "encode property");
    cJSON *arguments = json::add_action(actions, "set");
    if (!arguments) return ESP_ERR_NO_MEM;
    return json::add_boolean_argument(arguments, "on");
}

esp_err_t invoke_action(
    fabric_core_handle_t core, uint64_t node_id, uint16_t endpoint_id,
    const data_model::cluster_state *, const char *action,
    const cJSON *arguments, fabric_core_request_id_t *out_request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    if (!core || !action || !out_request_id || !completion_cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(action, "set")) return ESP_ERR_NOT_FOUND;
    const cJSON *on = json::only(arguments, "on");
    if (!cJSON_IsBool(on)) return ESP_ERR_INVALID_ARG;
    const fabric_core_command_path_t path = {
        .endpoint = {.node_id = node_id, .endpoint_id = endpoint_id},
        .cluster_id = kClusterId,
        .command_id = cJSON_IsTrue(on) ? UINT32_C(0x0001)
                                      : UINT32_C(0x0000),
    };
    return fabric_core_invoke_command(core, &path, "{}", out_request_id,
                                  completion_cb, user_ctx);
}

} // namespace

const data_model::cluster_adapter adapter = {
    .key = "on_off",
    .cluster_id = kClusterId,
    .decode_attribute = decode_attribute,
    .encode_json = encode_json,
    .invoke_action = invoke_action,
};

} // namespace data_model::clusters::on_off
