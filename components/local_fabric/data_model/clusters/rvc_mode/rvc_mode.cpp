/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rvc_mode.h"

#include "esp_check.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app/data-model/Decode.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace data_model::clusters::rvc_mode {
namespace {

constexpr uint32_t kRunModeClusterId = UINT32_C(0x0054);
constexpr uint32_t kCleanModeClusterId = UINT32_C(0x0055);
constexpr uint32_t kSupportedModesAttributeId = UINT32_C(0x0000);
constexpr uint32_t kCurrentModeAttributeId = UINT32_C(0x0001);
constexpr uint32_t kChangeToModeCommandId = UINT32_C(0x0000);

constexpr size_t kMaxModes = 8;
struct mode_option {
    uint8_t id;
    char label[18];
};
struct state {
    bool reported;
    bool modes_reported;
    uint8_t mode;
    uint8_t mode_count;
    mode_option modes[kMaxModes];
};
static_assert(sizeof(state) <= FABRIC_MANAGER_CLUSTER_STATE_MAX);

template <typename Modes>
CHIP_ERROR decode_supported_modes(chip::TLV::TLVReader &reader, state &current)
{
    Modes values;
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, values));
    state updated = current;
    updated.mode_count = 0;
    updated.modes_reported = true;
    auto iterator = values.begin();
    while (iterator.Next()) {
        if (updated.mode_count >= kMaxModes) {
            updated.modes_reported = false; // Never offer an incomplete picker.
            continue;
        }
        const auto &option = iterator.GetValue();
        for (size_t i = 0; i < updated.mode_count; ++i) {
            if (updated.modes[i].id == option.mode) {
                updated.modes_reported = false;
                break;
            }
        }
        auto &choice = updated.modes[updated.mode_count++];
        choice.id = option.mode;
        size_t length = option.label.size() < sizeof(choice.label)
                            ? option.label.size() : sizeof(choice.label) - 1;
        // Do not split a UTF-8 code point when shortening device text.
        if (length < option.label.size()) {
            while (length &&
                   (static_cast<unsigned char>(option.label.data()[length]) & 0xc0) == 0x80)
                --length;
        }
        memcpy(choice.label, option.label.data(), length);
        choice.label[length] = '\0';
        if (!length) snprintf(choice.label, sizeof(choice.label), "Mode %u", choice.id);
    }
    ReturnErrorOnFailure(iterator.GetStatus());
    current = updated;
    return CHIP_NO_ERROR;
}

template <uint32_t ClusterId>
CHIP_ERROR decode_attribute(uint32_t attribute_id,
                            chip::TLV::TLVReader &reader,
                            cluster_state *storage)
{
    auto &current = *reinterpret_cast<state *>(storage->bytes);
    if (attribute_id == kSupportedModesAttributeId) {
        if constexpr (ClusterId == kRunModeClusterId) {
            using Modes = chip::app::Clusters::RvcRunMode::Attributes::SupportedModes::TypeInfo::DecodableType;
            return decode_supported_modes<Modes>(reader, current);
        } else {
            using Modes = chip::app::Clusters::RvcCleanMode::Attributes::SupportedModes::TypeInfo::DecodableType;
            return decode_supported_modes<Modes>(reader, current);
        }
    }
    if (attribute_id != kCurrentModeAttributeId) return CHIP_ERROR_NOT_FOUND;
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, current.mode));
    current.reported = true;
    return CHIP_NO_ERROR;
}

const char *mode_label(const state &current, uint8_t id)
{
    for (size_t i = 0; i < current.mode_count; ++i)
        if (current.modes[i].id == id) return current.modes[i].label;
    return nullptr;
}

esp_err_t encode_json(const cluster_state *storage, cJSON *properties,
                      cJSON *actions)
{
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    const char *label = current.reported ? mode_label(current, current.mode) : nullptr;
    ESP_RETURN_ON_ERROR(
        json::add_property(properties, "mode",
                           current.reported ? cJSON_CreateNumber(current.mode)
                                            : cJSON_CreateNull()),
        "rvc_mode", "encode mode");
    if (current.reported) {
        cJSON *mode = cJSON_GetObjectItemCaseSensitive(properties, "mode");
        char fallback[18];
        snprintf(fallback, sizeof(fallback), "Mode %u", current.mode);
        if (!cJSON_AddStringToObject(mode, "label", label ? label : fallback))
            return ESP_ERR_NO_MEM;
    }
    if (!current.modes_reported || !current.mode_count) return ESP_OK;
    cJSON *arguments = json::add_action(actions, "set_mode");
    if (!arguments) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(json::add_integer_argument(arguments, "mode", 0, 255),
                        "rvc_mode", "encode mode argument");
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(arguments, "mode");
    cJSON *values = cJSON_AddArrayToObject(mode, "enum");
    if (!values) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < current.mode_count; ++i) {
        cJSON *choice = cJSON_CreateObject();
        if (!choice || !cJSON_AddNumberToObject(choice, "value", current.modes[i].id) ||
            !cJSON_AddStringToObject(choice, "label", current.modes[i].label)) {
            cJSON_Delete(choice);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(values, choice);
    }
    return ESP_OK;
}

template <uint32_t ClusterId>
esp_err_t invoke_action(
    fabric_core_handle_t core, uint64_t node_id, uint16_t endpoint_id,
    const cluster_state *storage, const char *action, const cJSON *arguments,
    fabric_core_request_id_t *out_request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    if (!core || !action || !out_request_id || !completion_cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(action, "set_mode")) return ESP_ERR_NOT_FOUND;
    int64_t mode = 0;
    if (!json::integer(json::only(arguments, "mode"), 0, 255, &mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    bool supported = false;
    if (current.modes_reported) {
        for (size_t i = 0; i < current.mode_count; ++i)
            supported |= current.modes[i].id == mode;
    }
    if (!supported) return ESP_ERR_INVALID_ARG;
    char command[24] = {};
    const int length = snprintf(command, sizeof(command), "{\"0:U8\":%u}",
                                static_cast<unsigned>(mode));
    if (length < 0 || static_cast<size_t>(length) >= sizeof(command)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const fabric_core_command_path_t path = {
        .endpoint = {.node_id = node_id, .endpoint_id = endpoint_id},
        .cluster_id = ClusterId,
        .command_id = kChangeToModeCommandId,
    };
    return fabric_core_invoke_command(core, &path, command, out_request_id,
                                  completion_cb, user_ctx);
}

} // namespace

const cluster_adapter run_mode_adapter = {
    .key = "rvc_run_mode",
    .cluster_id = kRunModeClusterId,
    .decode_attribute = decode_attribute<kRunModeClusterId>,
    .encode_json = encode_json,
    .invoke_action = invoke_action<kRunModeClusterId>,
};

const cluster_adapter clean_mode_adapter = {
    .key = "rvc_clean_mode",
    .cluster_id = kCleanModeClusterId,
    .decode_attribute = decode_attribute<kCleanModeClusterId>,
    .encode_json = encode_json,
    .invoke_action = invoke_action<kCleanModeClusterId>,
};

} // namespace data_model::clusters::rvc_mode
