/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "thermostat.h"

#include "esp_check.h"

#include <app/data-model/Decode.h>
#include <app/data-model/Nullable.h>

#include <cmath>
#include <cstring>

namespace data_model::clusters::thermostat {
namespace {

constexpr uint32_t kClusterId = UINT32_C(0x0201);
constexpr uint32_t kLocalTemperature = UINT32_C(0x0000);
constexpr uint32_t kCoolingSetpoint = UINT32_C(0x0011);
constexpr uint32_t kHeatingSetpoint = UINT32_C(0x0012);
constexpr uint32_t kSystemMode = UINT32_C(0x001c);
constexpr uint32_t kSequence = UINT32_C(0x001b);
constexpr uint32_t kMinHeat = UINT32_C(0x0015);
constexpr uint32_t kMaxHeat = UINT32_C(0x0016);
constexpr uint32_t kMinCool = UINT32_C(0x0017);
constexpr uint32_t kMaxCool = UINT32_C(0x0018);
constexpr uint32_t kDeadband = UINT32_C(0x0019);
constexpr uint32_t kFeatureMap = UINT32_C(0xfffc);

enum : uint16_t {
    kLocalReported = 1 << 0,
    kLocalNull = 1 << 1,
    kCoolingReported = 1 << 2,
    kHeatingReported = 1 << 3,
    kModeReported = 1 << 4,
    kSequenceReported = 1 << 5,
    kMinHeatReported = 1 << 6,
    kMaxHeatReported = 1 << 7,
    kMinCoolReported = 1 << 8,
    kMaxCoolReported = 1 << 9,
    kDeadbandReported = 1 << 10,
    kFeaturesReported = 1 << 11,
};

struct state {
    uint16_t flags;
    uint8_t mode;
    uint8_t sequence;
    int16_t local_temperature;
    int16_t cooling_setpoint;
    int16_t heating_setpoint;
    int16_t min_heat, max_heat, min_cool, max_cool;
    uint8_t deadband;
    uint32_t feature_map;
};
static_assert(sizeof(state) <= FABRIC_MANAGER_CLUSTER_STATE_MAX);

const char *mode_name(uint8_t mode)
{
    switch (mode) {
    case 0x00: return "off";
    case 0x01: return "auto";
    case 0x03: return "cool";
    case 0x04: return "heat";
    case 0x05: return "emergency_heat";
    case 0x06: return "precooling";
    case 0x07: return "fan_only";
    case 0x08: return "dry";
    case 0x09: return "sleep";
    default: return "unknown";
    }
}

bool mode_value(const char *name, uint8_t *out_mode)
{
    static constexpr struct {
        const char *name;
        uint8_t value;
    } modes[] = {{"off", 0x00},
                 {"auto", 0x01},
                 {"cool", 0x03},
                 {"heat", 0x04},
                 {"emergency_heat", 0x05},
                 {"precooling", 0x06},
                 {"fan_only", 0x07},
                 {"dry", 0x08},
                 {"sleep", 0x09}};
    for (const auto &mode : modes) {
        if (!strcmp(name, mode.name)) {
            *out_mode = mode.value;
            return true;
        }
    }
    return false;
}

bool mode_supported(const state &current, uint8_t mode)
{
    if (!(current.flags & kSequenceReported)) return false;
    if (mode == 0) return true;
    if (mode == 1) return current.sequence >= 4 &&
                          (current.flags & kFeaturesReported) &&
                          (current.feature_map & UINT32_C(0x20));
    if (mode == 3) return current.sequence == 0 || current.sequence == 1 ||
                          current.sequence >= 4;
    if (mode == 4) return current.sequence >= 2;
    return false;
}

bool setpoint_bounds(const state &current, bool heating,
                     double *minimum, double *maximum)
{
    const uint16_t required = heating ? kMinHeatReported | kMaxHeatReported
                                      : kMinCoolReported | kMaxCoolReported;
    if ((current.flags & (required | kModeReported)) !=
        (required | kModeReported)) return false;
    int16_t low = heating ? current.min_heat : current.min_cool;
    int16_t high = heating ? current.max_heat : current.max_cool;
    if (current.mode == 1) {
        uint16_t opposite = heating ? kCoolingReported : kHeatingReported;
        if ((current.flags & (kDeadbandReported | opposite)) !=
            (kDeadbandReported | opposite)) return false;
        int16_t gap = static_cast<int16_t>(current.deadband) * 10;
        if (heating && high > current.cooling_setpoint - gap)
            high = current.cooling_setpoint - gap;
        if (!heating && low < current.heating_setpoint + gap)
            low = current.heating_setpoint + gap;
    }
    if (low >= high) return false;
    *minimum = static_cast<double>(low) / 100.0;
    *maximum = static_cast<double>(high) / 100.0;
    return true;
}

CHIP_ERROR decode_attribute(uint32_t attribute_id,
                            chip::TLV::TLVReader &reader,
                            cluster_state *storage)
{
    auto &current = *reinterpret_cast<state *>(storage->bytes);
    if (attribute_id == kLocalTemperature) {
        chip::app::DataModel::Nullable<int16_t> value;
        ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, value));
        current.flags |= kLocalReported;
        current.flags = value.IsNull() ? current.flags | kLocalNull
                                       : current.flags & ~kLocalNull;
        current.local_temperature = value.IsNull() ? 0 : value.Value();
        return CHIP_NO_ERROR;
    }
    int16_t setpoint = 0;
    if (attribute_id == kCoolingSetpoint || attribute_id == kHeatingSetpoint) {
        ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, setpoint));
        if (attribute_id == kCoolingSetpoint) {
            current.cooling_setpoint = setpoint;
            current.flags |= kCoolingReported;
        } else {
            current.heating_setpoint = setpoint;
            current.flags |= kHeatingReported;
        }
        return CHIP_NO_ERROR;
    }
    if (attribute_id == kSystemMode) {
        ReturnErrorOnFailure(chip::app::DataModel::Decode(reader,
                                                          current.mode));
        current.flags |= kModeReported;
        return CHIP_NO_ERROR;
    }
    if (attribute_id == kFeatureMap) {
        ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, current.feature_map));
        current.flags |= kFeaturesReported;
        return CHIP_NO_ERROR;
    }
    if (attribute_id == kSequence) {
        ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, current.sequence));
        if (current.sequence > 5) return CHIP_ERROR_INVALID_ARGUMENT;
        current.flags |= kSequenceReported;
        return CHIP_NO_ERROR;
    }
    if (attribute_id == kDeadband) {
        ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, current.deadband));
        current.flags |= kDeadbandReported;
        return CHIP_NO_ERROR;
    }
    uint16_t flag = 0;
    int16_t *limit = nullptr;
    switch (attribute_id) {
    case kMinHeat: flag = kMinHeatReported; limit = &current.min_heat; break;
    case kMaxHeat: flag = kMaxHeatReported; limit = &current.max_heat; break;
    case kMinCool: flag = kMinCoolReported; limit = &current.min_cool; break;
    case kMaxCool: flag = kMaxCoolReported; limit = &current.max_cool; break;
    }
    if (limit) {
        ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, *limit));
        current.flags |= flag;
        return CHIP_NO_ERROR;
    }
    return CHIP_ERROR_NOT_FOUND;
}

esp_err_t add_temperature(cJSON *properties, const char *key, bool reported,
                          bool is_null, int16_t value)
{
    cJSON *json = reported && !is_null
                      ? cJSON_CreateNumber(static_cast<double>(value) / 100.0)
                      : cJSON_CreateNull();
    return json::add_property(properties, key, json, "celsius");
}

esp_err_t encode_json(const cluster_state *storage, cJSON *properties,
                      cJSON *actions)
{
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    ESP_RETURN_ON_ERROR(
        add_temperature(properties, "local_temperature_c",
                        current.flags & kLocalReported,
                        current.flags & kLocalNull,
                        current.local_temperature),
        "thermostat", "encode local temperature");
    if (current.flags & kModeReported) {
        ESP_RETURN_ON_ERROR(
            json::add_property(properties, "system_mode",
                               cJSON_CreateString(mode_name(current.mode))),
            "thermostat", "encode mode");
        static constexpr const char *modes[] = {
            "off", "auto", "cool", "heat",
        };
        static constexpr uint8_t ids[] = {0, 1, 3, 4};
        const char *supported[4] = {};
        size_t count = 0;
        for (size_t i = 0; i < 4; ++i)
            if (mode_supported(current, ids[i])) supported[count++] = modes[i];
        if (count) {
            cJSON *arguments = json::add_action(actions, "set_mode");
            if (!arguments) return ESP_ERR_NO_MEM;
            ESP_RETURN_ON_ERROR(
                json::add_string_argument(arguments, "mode", supported, count),
                "thermostat", "encode mode action");
        }
    } else {
        ESP_RETURN_ON_ERROR(
            json::add_property(properties, "system_mode", cJSON_CreateNull()),
            "thermostat", "encode empty mode");
    }
    if (current.flags & kHeatingReported) {
        ESP_RETURN_ON_ERROR(
            add_temperature(properties, "heating_setpoint_c", true, false,
                            current.heating_setpoint),
            "thermostat", "encode heating setpoint");
        double minimum = 0, maximum = 0;
        if (setpoint_bounds(current, true, &minimum, &maximum)) {
            cJSON *arguments = json::add_action(actions, "set_heating_setpoint");
            if (!arguments) return ESP_ERR_NO_MEM;
            ESP_RETURN_ON_ERROR(
                json::add_number_argument(arguments, "temperature_c",
                                          minimum, maximum, "celsius"),
                "thermostat", "encode heating action");
        }
    }
    if (current.flags & kCoolingReported) {
        ESP_RETURN_ON_ERROR(
            add_temperature(properties, "cooling_setpoint_c", true, false,
                            current.cooling_setpoint),
            "thermostat", "encode cooling setpoint");
        double minimum = 0, maximum = 0;
        if (setpoint_bounds(current, false, &minimum, &maximum)) {
            cJSON *arguments = json::add_action(actions, "set_cooling_setpoint");
            if (!arguments) return ESP_ERR_NO_MEM;
            ESP_RETURN_ON_ERROR(
                json::add_number_argument(arguments, "temperature_c",
                                          minimum, maximum, "celsius"),
                "thermostat", "encode cooling action");
        }
    }
    return ESP_OK;
}

esp_err_t write(fabric_core_handle_t core, uint64_t node_id,
                uint16_t endpoint_id, uint32_t attribute_id,
                const fabric_core_value_t &value,
                fabric_core_request_id_t *out_request_id,
                fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    const fabric_core_attribute_path_t path = {
        .endpoint = {.node_id = node_id, .endpoint_id = endpoint_id},
        .cluster_id = kClusterId,
        .attribute_id = attribute_id,
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
    if (!core || !action || !cJSON_IsObject(arguments) || !out_request_id ||
        !completion_cb) {
        return ESP_ERR_INVALID_ARG;
    }
    const auto &current = *reinterpret_cast<const state *>(storage->bytes);
    const cJSON *argument = nullptr;
    if (!strcmp(action, "set_mode")) {
        argument = json::only(arguments, "mode");
        if (!cJSON_IsString(argument)) return ESP_ERR_INVALID_ARG;
        uint8_t mode = 0;
        if (!mode_value(argument->valuestring, &mode) ||
            !mode_supported(current, mode)) return ESP_ERR_INVALID_ARG;
        const fabric_core_value_t value = {
            .type = FABRIC_CORE_VALUE_UNSIGNED,
            .data = {.unsigned_value = mode},
        };
        return write(core, node_id, endpoint_id, kSystemMode, value,
                     out_request_id, completion_cb, user_ctx);
    }
    uint32_t attribute_id = 0;
    if (!strcmp(action, "set_heating_setpoint")) {
        attribute_id = kHeatingSetpoint;
    } else if (!strcmp(action, "set_cooling_setpoint")) {
        attribute_id = kCoolingSetpoint;
    } else {
        return ESP_ERR_NOT_FOUND;
    }
    double minimum = 0, maximum = 0;
    if (!setpoint_bounds(current, attribute_id == kHeatingSetpoint,
                         &minimum, &maximum)) return ESP_ERR_NOT_SUPPORTED;
    double temperature = 0;
    if (!json::number(json::only(arguments, "temperature_c"), minimum, maximum,
                      &temperature)) {
        return ESP_ERR_INVALID_ARG;
    }
    const fabric_core_value_t value = {
        .type = FABRIC_CORE_VALUE_SIGNED,
        .data = {.signed_value = static_cast<int64_t>(
                     std::lround(temperature * 100.0))},
    };
    return write(core, node_id, endpoint_id, attribute_id, value,
                 out_request_id, completion_cb, user_ctx);
}

} // namespace

const cluster_adapter adapter = {
    .key = "thermostat",
    .cluster_id = kClusterId,
    .decode_attribute = decode_attribute,
    .encode_json = encode_json,
    .invoke_action = invoke_action,
};

} // namespace data_model::clusters::thermostat
