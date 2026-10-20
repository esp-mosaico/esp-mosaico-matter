/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "color_control.h"


#include "esp_check.h"

#include <app/data-model/Decode.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace data_model::clusters::color_control
{
namespace
{

constexpr uint32_t kClusterId = UINT32_C(0x0300);
constexpr uint32_t kHueSaturationFeature = UINT32_C(0x01);
constexpr uint32_t kXyFeature = UINT32_C(0x08);
constexpr uint32_t kColorTemperatureFeature = UINT32_C(0x10);
constexpr uint8_t kHueSaturationMode = UINT8_C(0x00);
constexpr uint8_t kXyMode = UINT8_C(0x01);
constexpr uint8_t kColorTemperatureMode = UINT8_C(0x02);
constexpr uint32_t kMicroMiredKelvinProduct = UINT32_C(1000000);
constexpr uint32_t kTemperatureKelvinMin = UINT32_C(2000);
constexpr uint32_t kTemperatureKelvinMax = UINT32_C(6500);

enum report_field : uint8_t {
    REPORT_FEATURE_MAP,
    REPORT_CURRENT_HUE,
    REPORT_CURRENT_SATURATION,
    REPORT_COLOR_MODE,
    REPORT_COLOR_TEMPERATURE_MIREDS,
    REPORT_COLOR_TEMPERATURE_MIREDS_MIN,
    REPORT_COLOR_TEMPERATURE_MIREDS_MAX,
    REPORT_CURRENT_X,
    REPORT_CURRENT_Y,
};

enum state_valid : uint16_t {
    VALID_FEATURE_MAP = UINT16_C(1) << 0,
    VALID_HUE = UINT16_C(1) << 1,
    VALID_SATURATION = UINT16_C(1) << 2,
    VALID_MODE = UINT16_C(1) << 3,
    VALID_TEMPERATURE = UINT16_C(1) << 4,
    VALID_TEMPERATURE_MIN = UINT16_C(1) << 5,
    VALID_TEMPERATURE_MAX = UINT16_C(1) << 6,
    VALID_X = UINT16_C(1) << 7,
    VALID_Y = UINT16_C(1) << 8,
};

struct state {
    uint32_t feature_map;
    uint16_t current_x;
    uint16_t current_y;
    uint16_t temperature_mireds;
    uint16_t temperature_mireds_min;
    uint16_t temperature_mireds_max;
    uint16_t valid;
    uint8_t current_hue;
    uint8_t current_saturation;
    uint8_t color_mode;
};
static_assert(sizeof(state) <= FABRIC_MANAGER_CLUSTER_STATE_MAX);

struct hsv {
    double hue_degrees;
    double saturation_percent;
    bool valid;
};

template <typename T>
CHIP_ERROR decode_number(chip::TLV::TLVReader &reader, T *output,
                         uint64_t minimum, uint64_t maximum)
{
    T value = 0;
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, value));
    VerifyOrReturnError(value >= minimum && value <= maximum,
                        CHIP_ERROR_INVALID_ARGUMENT);
    *output = value;
    return CHIP_NO_ERROR;
}

CHIP_ERROR decode_attribute(uint32_t attribute_id,
                            chip::TLV::TLVReader &reader,
                            data_model::cluster_state *storage)
{
    auto &color = *reinterpret_cast<state *>(storage->bytes);
    uint16_t valid = 0;
    CHIP_ERROR error = CHIP_NO_ERROR;
    switch (attribute_id) {
    case UINT32_C(0xfffc):
        error = decode_number(reader, &color.feature_map, 0, UINT32_MAX);
        valid = VALID_FEATURE_MAP;
        break;
    case UINT32_C(0x0000):
        error = decode_number(reader, &color.current_hue, 0, 254);
        valid = VALID_HUE;
        break;
    case UINT32_C(0x0001):
        error = decode_number(reader, &color.current_saturation, 0, 254);
        valid = VALID_SATURATION;
        break;
    case UINT32_C(0x0008):
        error = decode_number(reader, &color.color_mode, 0,
                              kColorTemperatureMode);
        valid = VALID_MODE;
        break;
    case UINT32_C(0x0007):
        error = decode_number(reader, &color.temperature_mireds, 1,
                              UINT16_MAX);
        valid = VALID_TEMPERATURE;
        break;
    case UINT32_C(0x400b):
        error = decode_number(reader, &color.temperature_mireds_min, 1,
                              UINT16_MAX);
        valid = VALID_TEMPERATURE_MIN;
        break;
    case UINT32_C(0x400c):
        error = decode_number(reader, &color.temperature_mireds_max, 1,
                              UINT16_MAX);
        valid = VALID_TEMPERATURE_MAX;
        break;
    case UINT32_C(0x0003):
        error = decode_number(reader, &color.current_x, 0, UINT16_C(0xfeff));
        valid = VALID_X;
        break;
    case UINT32_C(0x0004):
        error = decode_number(reader, &color.current_y, 0, UINT16_C(0xfeff));
        valid = VALID_Y;
        break;
    default:
        return CHIP_ERROR_NOT_FOUND;
    }
    ReturnErrorOnFailure(error);
    color.valid |= valid;
    if ((color.valid & (VALID_TEMPERATURE_MIN | VALID_TEMPERATURE_MAX)) ==
            (VALID_TEMPERATURE_MIN | VALID_TEMPERATURE_MAX) &&
        color.temperature_mireds_max < color.temperature_mireds_min) {
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
    return CHIP_NO_ERROR;
}

bool supports_hsv(const void *storage)
{
    const auto &color = *static_cast<const state *>(storage);
    return (color.feature_map & (kHueSaturationFeature | kXyFeature)) != 0;
}

bool supports_temperature(const void *storage)
{
    const auto &color = *static_cast<const state *>(storage);
    return (color.feature_map & kColorTemperatureFeature) != 0;
}

uint16_t hue_to_degrees(uint8_t value)
{
    return static_cast<uint16_t>((static_cast<uint32_t>(value) * 360U + 127U) /
                                 255U);
}

uint8_t saturation_to_percent(uint8_t value)
{
    return static_cast<uint8_t>((static_cast<uint32_t>(value) * 100U + 127U) /
                                254U);
}

double linear_to_srgb(double component)
{
    return component <= 0.0031308 ? 12.92 * component
                                  : 1.055 * std::pow(component, 1.0 / 2.4) - 0.055;
}

double srgb_to_linear(double component)
{
    return component <= 0.04045 ? component / 12.92
                                : std::pow((component + 0.055) / 1.055, 2.4);
}

hsv xy_to_hsv(const state &color)
{
    if ((color.valid & (VALID_X | VALID_Y)) != (VALID_X | VALID_Y) ||
        color.current_y == 0) {
        return {};
    }
    const double x = color.current_x / 65536.0;
    const double y = color.current_y / 65536.0;
    if (x < 0.0 || y <= 0.0 || x + y > 1.0) return {};
    const double cie_x = x / y;
    const double cie_y = 1.0;
    const double cie_z = (1.0 - x - y) / y;
    const double linear_red = 3.2406 * cie_x - 1.5372 * cie_y - 0.4986 * cie_z;
    const double linear_green = -0.9689 * cie_x + 1.8758 * cie_y + 0.0415 * cie_z;
    const double linear_blue = 0.0557 * cie_x - 0.2040 * cie_y + 1.0570 * cie_z;
    // XY contains chromaticity, not luminance. Normalize before converting
    // to sRGB so the arbitrary Y=1 does not clip channels and distort HSV.
    const double maximum_linear =
        std::max({linear_red, linear_green, linear_blue});
    if (maximum_linear <= 0.0) return {};
    const double red = linear_to_srgb(
        std::clamp(linear_red / maximum_linear, 0.0, 1.0));
    const double green = linear_to_srgb(
        std::clamp(linear_green / maximum_linear, 0.0, 1.0));
    const double blue = linear_to_srgb(
        std::clamp(linear_blue / maximum_linear, 0.0, 1.0));
    const double maximum = std::max({red, green, blue});
    const double minimum = std::min({red, green, blue});
    const double delta = maximum - minimum;
    double hue = 0.0;
    if (delta > 0.0) {
        if (maximum == red) hue = 60.0 * std::fmod((green - blue) / delta, 6.0);
        else if (maximum == green) hue = 60.0 * ((blue - red) / delta + 2.0);
        else hue = 60.0 * ((red - green) / delta + 4.0);
        if (hue < 0.0) hue += 360.0;
    }
    return {.hue_degrees = hue,
            .saturation_percent = maximum == 0.0 ? 0.0 : delta / maximum * 100.0,
            .valid = true};
}

bool hsv_to_xy(uint32_t hue_degrees, uint32_t saturation_percent,
               uint16_t *out_x, uint16_t *out_y)
{
    const double hue = hue_degrees / 60.0;
    const double saturation = saturation_percent / 100.0;
    const double chroma = saturation;
    const double intermediate =
        chroma * (1.0 - std::fabs(std::fmod(hue, 2.0) - 1.0));
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    switch (static_cast<unsigned>(hue) % 6U) {
    case 0: red = chroma; green = intermediate; break;
    case 1: red = intermediate; green = chroma; break;
    case 2: green = chroma; blue = intermediate; break;
    case 3: green = intermediate; blue = chroma; break;
    case 4: red = intermediate; blue = chroma; break;
    default: red = chroma; blue = intermediate; break;
    }
    const double match = 1.0 - chroma;
    red = srgb_to_linear(red + match);
    green = srgb_to_linear(green + match);
    blue = srgb_to_linear(blue + match);
    const double cie_x = 0.4124 * red + 0.3576 * green + 0.1805 * blue;
    const double cie_y = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
    const double cie_z = 0.0193 * red + 0.1192 * green + 0.9505 * blue;
    const double sum = cie_x + cie_y + cie_z;
    if (sum <= 0.0) return false;
    *out_x = static_cast<uint16_t>(
        std::clamp(std::lround(cie_x / sum * 65536.0), 0L, 0xfeffL));
    *out_y = static_cast<uint16_t>(
        std::clamp(std::lround(cie_y / sum * 65536.0), 0L, 0xfeffL));
    return true;
}

hsv current_hsv(const state &color)
{
    const bool xy_selected = (color.valid & VALID_MODE) != 0 &&
                             color.color_mode == kXyMode;
    if (xy_selected) {
        const hsv converted = xy_to_hsv(color);
        if (converted.valid) return converted;
    }
    if ((color.valid & (VALID_HUE | VALID_SATURATION)) ==
        (VALID_HUE | VALID_SATURATION)) {
        return {.hue_degrees = static_cast<double>(hue_to_degrees(color.current_hue)),
                .saturation_percent =
                    static_cast<double>(saturation_to_percent(
                        color.current_saturation)),
                .valid = true};
    }
    return xy_to_hsv(color);
}

cJSON *hue_to_json(const void *storage)
{
    const hsv color = current_hsv(*static_cast<const state *>(storage));
    return color.valid ? cJSON_CreateNumber(std::lround(color.hue_degrees))
                       : cJSON_CreateNull();
}

cJSON *saturation_to_json(const void *storage)
{
    const hsv color = current_hsv(*static_cast<const state *>(storage));
    return color.valid
               ? cJSON_CreateNumber(std::lround(color.saturation_percent))
               : cJSON_CreateNull();
}

uint32_t mireds_to_kelvin(uint16_t mireds)
{
    return (kMicroMiredKelvinProduct + mireds / 2U) / mireds;
}

cJSON *temperature_to_json(const void *storage)
{
    const auto &color = *static_cast<const state *>(storage);
    return (color.valid & VALID_TEMPERATURE) != 0
               ? cJSON_CreateNumber(mireds_to_kelvin(color.temperature_mireds))
               : cJSON_CreateNull();
}

void temperature_bounds(const state &color, uint32_t *out_minimum,
                        uint32_t *out_maximum)
{
    *out_minimum = kTemperatureKelvinMin;
    *out_maximum = kTemperatureKelvinMax;
    if ((color.valid & (VALID_TEMPERATURE_MIN | VALID_TEMPERATURE_MAX)) !=
        (VALID_TEMPERATURE_MIN | VALID_TEMPERATURE_MAX)) {
        return;
    }
    *out_minimum = std::max(
        kTemperatureKelvinMin,
        (kMicroMiredKelvinProduct + color.temperature_mireds_max - 1U) /
            color.temperature_mireds_max);
    *out_maximum = std::min(
        kTemperatureKelvinMax,
        kMicroMiredKelvinProduct / color.temperature_mireds_min);
}

esp_err_t submit_command(fabric_core_handle_t core, uint64_t node_id,
                         uint16_t endpoint_id, uint32_t command_id,
                         const char *command_json,
                         fabric_core_request_id_t *out_request_id,
                         fabric_core_request_completion_cb_t completion_cb,
                         void *user_ctx)
{
    const fabric_core_command_path_t path = {
        .endpoint = {.node_id = node_id, .endpoint_id = endpoint_id},
        .cluster_id = kClusterId,
        .command_id = command_id,
    };
    return fabric_core_invoke_command(core, &path, command_json, out_request_id,
                                  completion_cb, user_ctx);
}

esp_err_t invoke_color(fabric_core_handle_t core, uint64_t node_id,
                       uint16_t endpoint_id, const cJSON *arguments,
                       const void *storage,
                       fabric_core_request_id_t *out_request_id,
                       fabric_core_request_completion_cb_t completion_cb,
                       void *user_ctx)
{
    if (!core || !storage || !out_request_id || !completion_cb ||
        !cJSON_IsObject(arguments) || cJSON_GetArraySize(arguments) != 2) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t hue = 0;
    int64_t saturation = 0;
    if (!json::integer(
            cJSON_GetObjectItemCaseSensitive(arguments, "hue_degrees"), 0,
            359, &hue) ||
        !json::integer(cJSON_GetObjectItemCaseSensitive(
                           arguments, "saturation_percent"),
                       0, 100, &saturation)) {
        return ESP_ERR_INVALID_ARG;
    }
    const auto &color = *static_cast<const state *>(storage);
    char command[160] = {};
    uint32_t command_id = 0;
    int length = -1;
    if ((color.feature_map & kHueSaturationFeature) != 0) {
        command_id = UINT32_C(0x0006);
        const unsigned matter_hue = static_cast<unsigned>(std::min<long>(
            254L, std::lround(static_cast<double>(hue) * 255.0 / 360.0)));
        const unsigned matter_saturation = static_cast<unsigned>(
            std::lround(static_cast<double>(saturation) * 254.0 / 100.0));
        length = snprintf(command, sizeof(command),
                          "{\"0:U8\":%u,\"1:U8\":%u,\"2:U16\":0,"
                          "\"3:U8\":0,\"4:U8\":0}",
                          matter_hue, matter_saturation);
    } else if ((color.feature_map & kXyFeature) != 0) {
        command_id = UINT32_C(0x0007);
        uint16_t x = 0;
        uint16_t y = 0;
        if (!hsv_to_xy(static_cast<uint32_t>(hue),
                       static_cast<uint32_t>(saturation), &x, &y)) {
            return ESP_ERR_INVALID_ARG;
        }
        length = snprintf(command, sizeof(command),
                          "{\"0:U16\":%u,\"1:U16\":%u,\"2:U16\":0,"
                          "\"3:U8\":0,\"4:U8\":0}",
                          x, y);
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (length < 0 || static_cast<size_t>(length) >= sizeof(command)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return submit_command(core, node_id, endpoint_id, command_id, command,
                          out_request_id, completion_cb, user_ctx);
}

esp_err_t invoke_temperature(fabric_core_handle_t core, uint64_t node_id,
                             uint16_t endpoint_id, const cJSON *arguments,
                             const void *storage,
                             fabric_core_request_id_t *out_request_id,
                             fabric_core_request_completion_cb_t completion_cb,
                             void *user_ctx)
{
    if (!core || !storage || !out_request_id || !completion_cb) {
        return ESP_ERR_INVALID_ARG;
    }
    const auto &color = *static_cast<const state *>(storage);
    uint32_t minimum = 0;
    uint32_t maximum = 0;
    temperature_bounds(color, &minimum, &maximum);
    int64_t kelvin = 0;
    if (!json::integer(json::only(arguments, "color_temperature_kelvin"),
                       minimum, maximum, &kelvin)) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t mireds =
        (kMicroMiredKelvinProduct + static_cast<uint32_t>(kelvin) / 2U) /
        static_cast<uint32_t>(kelvin);
    char command[128] = {};
    const int length = snprintf(
        command, sizeof(command),
        "{\"0:U16\":%" PRIu32 ",\"1:U16\":0,\"2:U8\":0,"
        "\"3:U8\":0}",
        mireds);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(command)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return submit_command(core, node_id, endpoint_id, UINT32_C(0x000a),
                          command, out_request_id, completion_cb, user_ctx);
}

esp_err_t encode_json(const data_model::cluster_state *storage,
                      cJSON *properties, cJSON *actions)
{
    const void *state_bytes = storage->bytes;
    if (supports_hsv(state_bytes)) {
        ESP_RETURN_ON_ERROR(
            json::add_property(properties, "hue_degrees", hue_to_json(state_bytes),
                         "degrees"),
            "color_control", "encode hue");
        ESP_RETURN_ON_ERROR(
            json::add_property(properties, "saturation_percent",
                         saturation_to_json(state_bytes), "percent"),
            "color_control", "encode saturation");
        cJSON *arguments = json::add_action(actions, "set_color");
        if (!arguments) return ESP_ERR_NO_MEM;
        ESP_RETURN_ON_ERROR(
            json::add_integer_argument(arguments, "hue_degrees", 0, 359,
                                       "degrees"),
            "color_control", "encode hue argument");
        ESP_RETURN_ON_ERROR(
            json::add_integer_argument(arguments, "saturation_percent", 0,
                                       100, "percent"),
            "color_control", "encode saturation argument");
    }
    if (supports_temperature(state_bytes)) {
        const auto &color = *static_cast<const state *>(state_bytes);
        uint32_t minimum = 0;
        uint32_t maximum = 0;
        temperature_bounds(color, &minimum, &maximum);
        ESP_RETURN_ON_ERROR(
            json::add_property(properties, "color_temperature_kelvin",
                         temperature_to_json(state_bytes), "kelvin"),
            "color_control", "encode temperature");
        cJSON *arguments = json::add_action(actions, "set_color_temperature");
        if (!arguments) return ESP_ERR_NO_MEM;
        ESP_RETURN_ON_ERROR(
            json::add_integer_argument(arguments, "color_temperature_kelvin",
                                       minimum, maximum, "kelvin"),
            "color_control", "encode temperature argument");
    }
    return ESP_OK;
}

esp_err_t invoke_action(
    fabric_core_handle_t core, uint64_t node_id, uint16_t endpoint_id,
    const data_model::cluster_state *storage, const char *action,
    const cJSON *arguments, fabric_core_request_id_t *out_request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    if (!action) return ESP_ERR_INVALID_ARG;
    if (!strcmp(action, "set_color")) {
        return invoke_color(core, node_id, endpoint_id, arguments,
                            storage->bytes, out_request_id, completion_cb,
                            user_ctx);
    }
    if (!strcmp(action, "set_color_temperature")) {
        return invoke_temperature(core, node_id, endpoint_id, arguments,
                                  storage->bytes, out_request_id,
                                  completion_cb, user_ctx);
    }
    return ESP_ERR_NOT_FOUND;
}

} // namespace

const data_model::cluster_adapter adapter = {
    .key = "color_control",
    .cluster_id = kClusterId,
    .decode_attribute = decode_attribute,
    .encode_json = encode_json,
    .invoke_action = invoke_action,
};

} // namespace data_model::clusters::color_control
