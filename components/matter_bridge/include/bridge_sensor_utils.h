/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cmath>
#include <cstdint>

namespace bridge {

inline bool encode_illuminance(double lux, uint16_t &measured)
{
    if (!std::isfinite(lux) || lux < 0) return false;
    if (lux == 0) measured = 0;
    else {
        const double encoded = std::round(10000.0 * std::log10(lux) + 1.0);
        measured = static_cast<uint16_t>(encoded < 1 ? 1 : encoded > 65534 ? 65534 : encoded);
    }
    return true;
}

struct sensor_report_t {
    bool initialized;
    bool is_null;
    uint16_t value;

    bool changed(bool next_null, uint16_t next_value) const
    {
        return !initialized || is_null != next_null || (!next_null && value != next_value);
    }
    void remember(bool next_null, uint16_t next_value)
    {
        initialized = true;
        is_null = next_null;
        value = next_value;
    }
};

} // namespace bridge
