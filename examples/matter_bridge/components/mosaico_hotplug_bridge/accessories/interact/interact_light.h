/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <math.h>
#include <stdbool.h>

// GL5506: use the 4-7 kohm midpoint at 10 lux and gamma = 0.5.
#define MOSAICO_INTERACT_LIGHT_R10_OHMS 5500
#define MOSAICO_INTERACT_LIGHT_GAMMA_X1000 500

// ESP-Mosaico Interaction V1.0: 3.3 V -> GL5506 -> ADC -> 2 kohm -> GND.
// S31's nominal single-ended positive input range is 0..2 V. The current
// S31 IDF has no voltage calibration scheme; these are nominal voltages.
static inline bool mosaico_interact_s31_light_voltage(int raw, unsigned channel, double *millivolts)
{
    if (!millivolts || raw < 0 || raw > 4393) return false;
    const bool inverted = (channel & 1U) == 0;
    int level = inverted ? 2196 - raw : raw - 2196;
    const int span = inverted ? 2196 : 4393 - 2196;
    // Report saturation at the measurable upper bound (about 179 lux with
    // the default LDR model), rather than treating strong light as unknown.
    if (level > span) level = span;
    if (level < 0) level = 0;
    *millivolts = 2000.0 * level / span;
    return true;
}

static inline bool mosaico_interact_light_estimate_lux(double millivolts, double r10_ohms,
                                                      double gamma, double *lux)
{
    if (!lux || !isfinite(millivolts) || millivolts < 0 || millivolts > 2000 ||
        !isfinite(r10_ohms) || r10_ohms <= 0 || !isfinite(gamma) || gamma <= 0) return false;
    if (millivolts == 0) {
        *lux = 0;
        return true;
    }
    const double resistance = 2000.0 * (3300.0 / millivolts - 1.0);
    // GL5506: R(E) = R10 * (10 / E)^gamma; gamma is specified at 10..100 lux.
    const double estimate = 10.0 * pow(r10_ohms / resistance, 1.0 / gamma);
    if (!isfinite(estimate)) return false;
    *lux = estimate;
    return true;
}
