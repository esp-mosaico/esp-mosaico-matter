/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "level_control.h"


#include "esp_check.h"

#include <app/data-model/Decode.h>
#include <app/data-model/Nullable.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace data_model::clusters::level_control
{
namespace
{

constexpr uint32_t kClusterId = UINT32_C(0x0008);
constexpr uint32_t kCurrentLevelAttributeId = UINT32_C(0x0000);

struct state {
    bool reported;
    bool is_null;
    uint8_t level;
};
static_assert(sizeof(state) <= FABRIC_MANAGER_CLUSTER_STATE_MAX);

CHIP_ERROR decode_attribute(uint32_t attribute_id,
                            chip::TLV::TLVReader &reader,
                            data_model::cluster_state *storage)
{
    if (attribute_id != kCurrentLevelAttributeId) return CHIP_ERROR_NOT_FOUND;
    chip::app::DataModel::Nullable<uint8_t> level;
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, level));
    VerifyOrReturnError(level.IsNull() ||
                            (level.Value() >= 1 && level.Value() <= 254),
                        CHIP_ERROR_INVALID_ARGUMENT);
    auto &current = *reinterpret_cast<state *>(storage->bytes);
    current.reported = true;
    current.is_null = level.IsNull();
    current.level = level.IsNull() ? 0 : level.Value();
    return CHIP_NO_ERROR;
}

esp_err_t encode_json(const data_model::cluster_state *storage, cJSON *properties,
                      cJSON *actions)
{
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    const uint32_t percent = std::max<uint32_t>(1,
        (static_cast<uint32_t>(current.level) * 100U + 127U) / 254U);
    cJSON *value = current.reported && !current.is_null
                       ? cJSON_CreateNumber(percent)
                       : cJSON_CreateNull();
    ESP_RETURN_ON_ERROR(json::add_property(properties, "level_percent", value,
                                     "percent"),
                        "level_control", "encode property");
    cJSON *arguments = json::add_action(actions, "set_level");
    if (!arguments) return ESP_ERR_NO_MEM;
    return json::add_integer_argument(arguments, "level_percent", 1, 100,
                                      "percent");
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
    if (strcmp(action, "set_level")) return ESP_ERR_NOT_FOUND;
    int64_t percent = 0;
    if (!json::integer(json::only(arguments, "level_percent"), 1, 100,
                       &percent)) {
        return ESP_ERR_INVALID_ARG;
    }
    const unsigned level = static_cast<unsigned>(std::max<long>(
        1L, std::lround(static_cast<double>(percent) * 254.0 / 100.0)));
    char command[96] = {};
    const int length = snprintf(
        command, sizeof(command),
        "{\"0:U8\":%u,\"1:NULL\":null,\"2:U8\":0,\"3:U8\":0}",
        level);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(command)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const fabric_core_command_path_t path = {
        .endpoint = {.node_id = node_id, .endpoint_id = endpoint_id},
        .cluster_id = kClusterId,
        .command_id = UINT32_C(0x0004), // MoveToLevelWithOnOff
    };
    return fabric_core_invoke_command(core, &path, command, out_request_id,
                                  completion_cb, user_ctx);
}

} // namespace

const data_model::cluster_adapter adapter = {
    .key = "level_control",
    .cluster_id = kClusterId,
    .decode_attribute = decode_attribute,
    .encode_json = encode_json,
    .invoke_action = invoke_action,
};

} // namespace data_model::clusters::level_control
