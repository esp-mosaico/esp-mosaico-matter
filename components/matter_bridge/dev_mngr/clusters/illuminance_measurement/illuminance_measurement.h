/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "bridge_dev_mngr_private.h"

namespace bridge::clusters::illuminance_measurement {

constexpr uint32_t kClusterId = UINT32_C(0x0400);
extern const cluster_adapter adapter;

} // namespace bridge::clusters::illuminance_measurement
