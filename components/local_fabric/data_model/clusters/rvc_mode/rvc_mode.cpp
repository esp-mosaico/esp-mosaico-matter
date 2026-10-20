/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rvc_mode.h"

#include "esp_check.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app/data-model/Decode.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace data_model::clusters::rvc_mode {
namespace {

constexpr uint32_t kRunModeClusterId = UINT32_C(0x0054);
constexpr uint32_t kCleanModeClusterId = UINT32_C(0x0055);
constexpr uint32_t kSupportedModesAttributeId = UINT32_C(0x0000);
constexpr uint32_t kCurrentModeAttributeId = UINT32_C(0x0001);
constexpr uint32_t kChangeToModeCommandId = UINT32_C(0x0000);

struct state {
    bool reported;
    uint8_t mode;
};
static_assert(sizeof(state) <= FABRIC_MANAGER_CLUSTER_STATE_MAX);

const cJSON *find_mode(const cJSON *modes, uint8_t id)
{
    const cJSON *choice = nullptr;
    cJSON_ArrayForEach(choice, modes) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(choice, "value");
        if (value && value->valueint == id) return choice;
    }
    return nullptr;
}

template <typename Modes>
CHIP_ERROR decode_supported_modes(chip::TLV::TLVReader &reader,
                                   cluster_state *storage)
{
    Modes values;
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, values));
    using json_owner = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    json_owner modes(cJSON_CreateArray(), cJSON_Delete);
    VerifyOrReturnError(modes, CHIP_ERROR_NO_MEMORY);
    auto iterator = values.begin();
    while (iterator.Next()) {
        const auto &option = iterator.GetValue();
        // Matter ModeOptionStruct labels are at most 64 UTF-8 bytes.
        char label[65];
        VerifyOrReturnError(option.label.size() < sizeof(label),
                            CHIP_ERROR_INVALID_TLV_ELEMENT);
        memcpy(label, option.label.data(), option.label.size());
        label[option.label.size()] = '\0';
        if (!option.label.size()) snprintf(label, sizeof(label), "Mode %u", option.mode);
        const cJSON *previous = find_mode(modes.get(), option.mode);
        if (previous) {
            const cJSON *previous_label = cJSON_GetObjectItemCaseSensitive(previous, "label");
            VerifyOrReturnError(!strcmp(previous_label->valuestring, label),
                                CHIP_ERROR_INVALID_TLV_ELEMENT);
            continue;
        }
        json_owner choice(cJSON_CreateObject(), cJSON_Delete);
        VerifyOrReturnError(choice &&
                            cJSON_AddNumberToObject(choice.get(), "value", option.mode) &&
                            cJSON_AddStringToObject(choice.get(), "label", label) &&
                            cJSON_AddItemToArray(modes.get(), choice.get()),
                            CHIP_ERROR_NO_MEMORY);
        choice.release();
    }
    ReturnErrorOnFailure(iterator.GetStatus());
    VerifyOrReturnError(storage->metadata.reset(modes.release()), CHIP_ERROR_NO_MEMORY);
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
            return decode_supported_modes<Modes>(reader, storage);
        } else {
            using Modes = chip::app::Clusters::RvcCleanMode::Attributes::SupportedModes::TypeInfo::DecodableType;
            return decode_supported_modes<Modes>(reader, storage);
        }
    }
    if (attribute_id != kCurrentModeAttributeId) return CHIP_ERROR_NOT_FOUND;
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, current.mode));
    current.reported = true;
    return CHIP_NO_ERROR;
}

esp_err_t encode_json(const cluster_state *storage, cJSON *properties,
                      cJSON *actions)
{
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    const cJSON *modes = storage->metadata.get();
    const cJSON *choice = current.reported ? find_mode(modes, current.mode) : nullptr;
    const cJSON *label = choice ? cJSON_GetObjectItemCaseSensitive(choice, "label") : nullptr;
    ESP_RETURN_ON_ERROR(
        json::add_property(properties, "mode",
                           current.reported ? cJSON_CreateNumber(current.mode)
                                            : cJSON_CreateNull()),
        "rvc_mode", "encode mode");
    if (current.reported) {
        cJSON *mode = cJSON_GetObjectItemCaseSensitive(properties, "mode");
        char fallback[18];
        snprintf(fallback, sizeof(fallback), "Mode %u", current.mode);
        if (!cJSON_AddStringToObject(mode, "label", label ? label->valuestring : fallback))
            return ESP_ERR_NO_MEM;
    }
    if (!modes || !cJSON_GetArraySize(modes)) return ESP_OK;
    cJSON *arguments = json::add_action(actions, "set_mode");
    if (!arguments) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(json::add_integer_argument(arguments, "mode", 0, 255),
                        "rvc_mode", "encode mode argument");
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(arguments, "mode");
    cJSON *values = cJSON_Duplicate(modes, true);
    if (!values || !cJSON_AddItemToObject(mode, "enum", values)) {
        cJSON_Delete(values);
        return ESP_ERR_NO_MEM;
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
    if (!find_mode(storage->metadata.get(), static_cast<uint8_t>(mode)))
        return ESP_ERR_INVALID_ARG;
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
