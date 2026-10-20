/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "bridge_dev_mngr_private.h"

namespace bridge::clusters::occupancy_sensing {

constexpr uint32_t kClusterId = UINT32_C(0x0406);
constexpr uint32_t kOccupancyAttributeId = UINT32_C(0x0000);
extern const cluster_adapter adapter;

} // namespace bridge::clusters::occupancy_sensing
