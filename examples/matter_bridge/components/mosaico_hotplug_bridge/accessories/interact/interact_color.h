/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <algorithm>
#include <cmath>
#include "bridge_dev_mngr.h"

namespace mosaico_interact {

struct color_state {
    bool on = false;
    uint8_t level = 254;
    uint8_t hue = 0;
    uint8_t saturation = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t temperature = 250;
    uint8_t mode = 0;
};

struct rgb {
    uint8_t red = 0, green = 0, blue = 0;
    bool operator==(const rgb &other) const
    { return red == other.red && green == other.green && blue == other.blue; }
    bool operator!=(const rgb &other) const { return !(*this == other); }
};

// Attribute units remain Matter's raw units. The physical driver alone maps
// CurrentLevel's 0..254 range to WS2812's full 0..255 channel range.
inline constexpr bridge_dev_mngr_attribute_path_t color_paths[] = {
    {0x0006, 0}, {0x0008, 0}, {0x0300, 0}, {0x0300, 1},
    {0x0300, 3}, {0x0300, 4}, {0x0300, 7}, {0x0300, 8}, {0x0300, 0x4001},
};
inline constexpr uint32_t initial_color_values[] = {0, 254, 0, 0, 0, 0, 250, 0, 0};

inline bool apply_color_attribute(color_state &state,
                                   const bridge_dev_mngr_attribute_path_t &path,
                                   const bridge_dev_mngr_value_t &value)
{
    if (value.truncated) return false;
    if (path.cluster_id == 0x0006 && path.attribute_id == 0) {
        if (value.type != BRIDGE_DEV_MNGR_VALUE_BOOL) return false;
        state.on = value.data.boolean;
        return true;
    }
    if (value.type != BRIDGE_DEV_MNGR_VALUE_UNSIGNED) return false;
    const uint64_t number = value.data.unsigned_value;
    if (path.cluster_id == 0x0008 && path.attribute_id == 0 && number <= 254) {
        state.level = static_cast<uint8_t>(number);
        return true;
    }
    if (path.cluster_id != 0x0300) return false;
    switch (path.attribute_id) {
    case 0:
        if (number > 254) return false;
        state.hue = static_cast<uint8_t>(number);
        return true;
    case 1:
        if (number > 254) return false;
        state.saturation = static_cast<uint8_t>(number);
        return true;
    case 3:
    case 4:
        if (number > UINT16_MAX) return false;
        (path.attribute_id == 3 ? state.x : state.y) = static_cast<uint16_t>(number);
        return true;
    case 7:
        // Match the existing color_light profile's physical mired bounds.
        if (number < 153 || number > 500) return false;
        state.temperature = static_cast<uint16_t>(number);
        return true;
    case 8:
        if (number > 2) return false;
        state.mode = static_cast<uint8_t>(number);
        return true;
    case 0x4001:
        // ColorMode is authoritative for the non-enhanced HS/XY/CT features.
        // Reading its mirror later must not overwrite a newer mode command.
        return number <= 2;
    default:
        return false;
    }
}

inline rgb to_rgb(const color_state &state)
{
    if (!state.on || !state.level) return {};
    double r, g, b;
    if (state.mode == 1) {
        // Same normalized sRGB matrix and gamma as matter_bridge_color.lua.
        if (!state.y) return {};
        const double x = state.x / 65536.0, y = state.y / 65536.0;
        const double X = x / y, Z = (1 - x - y) / y;
        r = std::max(0.0, 3.2406 * X - 1.5372 - 0.4986 * Z);
        g = std::max(0.0, -0.9689 * X + 1.8758 + 0.0415 * Z);
        b = std::max(0.0, 0.0557 * X - 0.2040 + 1.0570 * Z);
        const double peak = std::max({1.0, r, g, b});
        const auto gamma = [peak](double value) {
            value /= peak;
            return value <= 0.0031308 ? 12.92 * value :
                   1.055 * std::pow(value, 1 / 2.4) - 0.055;
        };
        r = gamma(r); g = gamma(g); b = gamma(b);
    } else if (state.mode == 2) {
        // Approximate a white point using RGB; there is no separate white LED.
        const double t = std::clamp(1000000.0 / std::max<uint16_t>(1, state.temperature),
                                    2000.0, 6500.0) / 100;
        r = t <= 66 ? 255 : 329.698727446 * std::pow(t - 60, -0.1332047592);
        g = t <= 66 ? 99.4708025861 * std::log(t) - 161.1195681661 :
                     288.1221695283 * std::pow(t - 60, -0.0755148492);
        b = t >= 66 ? 255 : t <= 19 ? 0 :
                     138.5177312231 * std::log(t - 10) - 305.0447927307;
        r /= 255; g /= 255; b /= 255;
    } else {
        const double h = std::min<uint8_t>(254, state.hue) / 254.0 * 6;
        const double c = std::min<uint8_t>(254, state.saturation) / 254.0;
        const double x = c * (1 - std::abs(std::fmod(h, 2.0) - 1));
        if (h < 1) { r = c; g = x; b = 0; }
        else if (h < 2) { r = x; g = c; b = 0; }
        else if (h < 3) { r = 0; g = c; b = x; }
        else if (h < 4) { r = 0; g = x; b = c; }
        else if (h < 5) { r = x; g = 0; b = c; }
        else { r = c; g = 0; b = x; }
        r += 1 - c; g += 1 - c; b += 1 - c;
    }
    const double level = std::min<uint8_t>(254, state.level) / 254.0;
    const auto channel = [level](double value) {
        return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * level * 255));
    };
    return {channel(r), channel(g), channel(b)};
}

} // namespace mosaico_interact
