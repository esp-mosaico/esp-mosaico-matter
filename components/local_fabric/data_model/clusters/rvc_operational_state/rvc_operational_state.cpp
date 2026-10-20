/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rvc_operational_state.h"

#include "esp_check.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app/data-model/Decode.h>

#include <cstring>

namespace data_model::clusters::rvc_operational_state {
namespace {

constexpr uint32_t kClusterId = UINT32_C(0x0061);
constexpr uint32_t kOperationalStateAttributeId = UINT32_C(0x0004);
constexpr uint32_t kPauseCommandId = UINT32_C(0x0000);
constexpr uint32_t kResumeCommandId = UINT32_C(0x0003);
constexpr uint32_t kGoHomeCommandId = UINT32_C(0x0080);
constexpr uint32_t kAcceptedCommandListAttributeId = UINT32_C(0xfff9);

struct state {
    bool reported;
    uint8_t value;
    uint8_t command_mask;
};
static_assert(sizeof(state) <= FABRIC_MANAGER_CLUSTER_STATE_MAX);

const char *state_name(uint8_t value)
{
    switch (value) {
    case 0x00: return "stopped";
    case 0x01: return "running";
    case 0x02: return "paused";
    case 0x03: return "error";
    case 0x40: return "seeking_charger";
    case 0x41: return "charging";
    case 0x42: return "docked";
    case 0x43: return "emptying_dust_bin";
    case 0x44: return "cleaning_mop";
    case 0x45: return "filling_water_tank";
    case 0x46: return "updating_maps";
    default: return "unknown";
    }
}

CHIP_ERROR decode_attribute(uint32_t attribute_id,
                            chip::TLV::TLVReader &reader,
                            cluster_state *storage)
{
    auto &current = *reinterpret_cast<state *>(storage->bytes);
    if (attribute_id == kAcceptedCommandListAttributeId) {
        chip::app::Clusters::RvcOperationalState::Attributes::AcceptedCommandList::TypeInfo::DecodableType commands;
        ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, commands));
        uint8_t mask = 0;
        auto iterator = commands.begin();
        while (iterator.Next()) {
            switch (iterator.GetValue()) {
            case kPauseCommandId: mask |= 1; break;
            case kResumeCommandId: mask |= 2; break;
            case kGoHomeCommandId: mask |= 4; break;
            }
        }
        ReturnErrorOnFailure(iterator.GetStatus());
        current.command_mask = mask;
        return CHIP_NO_ERROR;
    }
    if (attribute_id != kOperationalStateAttributeId) {
        return CHIP_ERROR_NOT_FOUND;
    }
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, current.value));
    current.reported = true;
    return CHIP_NO_ERROR;
}

esp_err_t encode_json(const cluster_state *storage, cJSON *properties,
                      cJSON *actions)
{
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    ESP_RETURN_ON_ERROR(
        json::add_property(properties, "state",
                           current.reported
                               ? cJSON_CreateString(state_name(current.value))
                               : cJSON_CreateNull()),
        "rvc_operational_state", "encode state");
    if ((current.command_mask & 1 && !json::add_action(actions, "pause")) ||
        (current.command_mask & 2 && !json::add_action(actions, "resume")) ||
        (current.command_mask & 4 && !json::add_action(actions, "go_home"))) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t invoke_action(
    fabric_core_handle_t core, uint64_t node_id, uint16_t endpoint_id,
    const cluster_state *storage, const char *action, const cJSON *arguments,
    fabric_core_request_id_t *out_request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    if (!core || !action || !cJSON_IsObject(arguments) || arguments->child ||
        !out_request_id || !completion_cb) {
        return ESP_ERR_INVALID_ARG;
    }
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    uint32_t command_id = 0;
    uint8_t required_mask = 0;
    if (!strcmp(action, "pause")) {
        command_id = kPauseCommandId;
        required_mask = 1;
    } else if (!strcmp(action, "resume")) {
        command_id = kResumeCommandId;
        required_mask = 2;
    } else if (!strcmp(action, "go_home")) {
        command_id = kGoHomeCommandId;
        required_mask = 4;
    } else {
        return ESP_ERR_NOT_FOUND;
    }
    if (!(current.command_mask & required_mask)) return ESP_ERR_NOT_SUPPORTED;
    const fabric_core_command_path_t path = {
        .endpoint = {.node_id = node_id, .endpoint_id = endpoint_id},
        .cluster_id = kClusterId,
        .command_id = command_id,
    };
    return fabric_core_invoke_command(core, &path, "{}", out_request_id,
                                  completion_cb, user_ctx);
}

} // namespace

const cluster_adapter adapter = {
    .key = "rvc_operational_state",
    .cluster_id = kClusterId,
    .decode_attribute = decode_attribute,
    .encode_json = encode_json,
    .invoke_action = invoke_action,
};

} // namespace data_model::clusters::rvc_operational_state
