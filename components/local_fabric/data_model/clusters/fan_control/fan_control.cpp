/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fan_control.h"

#include "esp_check.h"

#include <app/data-model/Decode.h>
#include <app/data-model/Nullable.h>

#include <cmath>
#include <cstring>

namespace data_model::clusters::fan_control {
namespace {

constexpr uint32_t kClusterId = UINT32_C(0x0202);
constexpr uint32_t kFanMode = UINT32_C(0x0000);
constexpr uint32_t kFanModeSequence = UINT32_C(0x0001);
constexpr uint32_t kPercentSetting = UINT32_C(0x0002);
constexpr uint32_t kPercentCurrent = UINT32_C(0x0003);

enum : uint8_t {
    kModeReported = 1 << 0,
    kSettingReported = 1 << 1,
    kSettingNull = 1 << 2,
    kCurrentReported = 1 << 3,
    kSequenceReported = 1 << 4,
};

struct state {
    uint8_t flags;
    uint8_t mode;
    uint8_t setting;
    uint8_t current;
    uint8_t sequence;
};
static_assert(sizeof(state) <= FABRIC_MANAGER_CLUSTER_STATE_MAX);

const char *mode_name(uint8_t mode)
{
    static constexpr const char *names[] = {
        "off", "low", "medium", "high", "on", "auto", "smart"};
    return mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "unknown";
}

bool mode_value(const char *name, uint8_t *out_mode)
{
    for (uint8_t mode = 0; mode <= 6; ++mode) {
        if (!strcmp(name, mode_name(mode))) {
            *out_mode = mode;
            return true;
        }
    }
    return false;
}

bool mode_supported(const state &current, uint8_t mode)
{
    if (!(current.flags & kSequenceReported) || current.sequence > 5) return false;
    if (mode == 0) return true;
    if (mode == 1) return current.sequence <= 3;
    if (mode == 2) return current.sequence == 0 || current.sequence == 2;
    if (mode == 3) return true;
    if (mode == 5) return current.sequence == 2 || current.sequence == 3 ||
                          current.sequence == 4;
    return false;
}

CHIP_ERROR decode_attribute(uint32_t attribute_id,
                            chip::TLV::TLVReader &reader,
                            cluster_state *storage)
{
    auto &current = *reinterpret_cast<state *>(storage->bytes);
    if (attribute_id == kFanModeSequence) {
        ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, current.sequence));
        VerifyOrReturnError(current.sequence <= 5, CHIP_ERROR_INVALID_ARGUMENT);
        current.flags |= kSequenceReported;
        return CHIP_NO_ERROR;
    }
    if (attribute_id == kFanMode) {
        ReturnErrorOnFailure(chip::app::DataModel::Decode(reader,
                                                          current.mode));
        VerifyOrReturnError(current.mode <= 6, CHIP_ERROR_INVALID_ARGUMENT);
        current.flags |= kModeReported;
        return CHIP_NO_ERROR;
    }
    if (attribute_id == kPercentSetting) {
        chip::app::DataModel::Nullable<uint8_t> value;
        ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, value));
        VerifyOrReturnError(value.IsNull() || value.Value() <= 100,
                            CHIP_ERROR_INVALID_ARGUMENT);
        current.setting = value.IsNull() ? 0 : value.Value();
        current.flags |= kSettingReported;
        current.flags = value.IsNull() ? current.flags | kSettingNull
                                       : current.flags & ~kSettingNull;
        return CHIP_NO_ERROR;
    }
    if (attribute_id == kPercentCurrent) {
        ReturnErrorOnFailure(chip::app::DataModel::Decode(reader,
                                                          current.current));
        VerifyOrReturnError(current.current <= 100,
                            CHIP_ERROR_INVALID_ARGUMENT);
        current.flags |= kCurrentReported;
        return CHIP_NO_ERROR;
    }
    return CHIP_ERROR_NOT_FOUND;
}

esp_err_t encode_json(const cluster_state *storage, cJSON *properties,
                      cJSON *actions)
{
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    cJSON *mode = current.flags & kModeReported
                      ? cJSON_CreateString(mode_name(current.mode))
                      : cJSON_CreateNull();
    ESP_RETURN_ON_ERROR(json::add_property(properties, "mode", mode),
                        "fan_control", "encode mode");
    const bool has_percent = (current.flags & kSettingReported) &&
                             !(current.flags & kSettingNull);
    const uint8_t percent = current.setting;
    ESP_RETURN_ON_ERROR(
        json::add_property(properties, "percent",
                           has_percent ? cJSON_CreateNumber(percent)
                                       : cJSON_CreateNull(),
                           "percent"),
        "fan_control", "encode percent");

    if ((current.flags & kModeReported) && (current.flags & kSequenceReported)) {
        static constexpr const char *modes[] = {
            "off", "low", "medium", "high", "on", "auto", "smart"};
        const char *supported[7] = {};
        size_t count = 0;
        for (uint8_t value = 0; value < 7; ++value)
            if (mode_supported(current, value)) supported[count++] = modes[value];
        cJSON *arguments = json::add_action(actions, "set_mode");
        if (!arguments) return ESP_ERR_NO_MEM;
        ESP_RETURN_ON_ERROR(
            json::add_string_argument(arguments, "mode", supported, count),
            "fan_control", "encode mode action");
    }
    if (has_percent) {
        cJSON *arguments = json::add_action(actions, "set_percent");
        if (!arguments) return ESP_ERR_NO_MEM;
        ESP_RETURN_ON_ERROR(
            json::add_integer_argument(arguments, "percent", 0, 100,
                                       "percent"),
            "fan_control", "encode percent action");
    }
    return ESP_OK;
}

esp_err_t write(fabric_core_handle_t core, uint64_t node_id,
                uint16_t endpoint_id, uint32_t attribute_id, uint8_t number,
                fabric_core_request_id_t *out_request_id,
                fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    const fabric_core_attribute_path_t path = {
        .endpoint = {.node_id = node_id, .endpoint_id = endpoint_id},
        .cluster_id = kClusterId,
        .attribute_id = attribute_id,
    };
    const fabric_core_value_t value = {
        .type = FABRIC_CORE_VALUE_UNSIGNED,
        .data = {.unsigned_value = number},
    };
    return fabric_core_write_attribute(core, &path, &value, out_request_id,
                                   completion_cb, user_ctx);
}

esp_err_t invoke_action(
    fabric_core_handle_t core, uint64_t node_id, uint16_t endpoint_id,
    const cluster_state *storage, const char *action, const cJSON *arguments,
    fabric_core_request_id_t *out_request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    if (!core || !action || !out_request_id || !completion_cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!strcmp(action, "set_mode")) {
        const cJSON *mode = json::only(arguments, "mode");
        uint8_t value = 0;
        if (!cJSON_IsString(mode) ||
            !mode_value(mode->valuestring, &value) ||
            !mode_supported(*reinterpret_cast<const state *>(storage->bytes), value)) {
            return ESP_ERR_INVALID_ARG;
        }
        return write(core, node_id, endpoint_id, kFanMode, value,
                     out_request_id, completion_cb, user_ctx);
    }
    if (strcmp(action, "set_percent")) return ESP_ERR_NOT_FOUND;
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    if (!(current.flags & kSettingReported) || (current.flags & kSettingNull))
        return ESP_ERR_NOT_SUPPORTED;
    int64_t percent = 0;
    if (!json::integer(json::only(arguments, "percent"), 0, 100,
                       &percent)) {
        return ESP_ERR_INVALID_ARG;
    }
    return write(core, node_id, endpoint_id, kPercentSetting,
                 static_cast<uint8_t>(percent), out_request_id,
                 completion_cb, user_ctx);
}

} // namespace

const cluster_adapter adapter = {
    .key = "fan_control",
    .cluster_id = kClusterId,
    .decode_attribute = decode_attribute,
    .encode_json = encode_json,
    .invoke_action = invoke_action,
};

} // namespace data_model::clusters::fan_control
