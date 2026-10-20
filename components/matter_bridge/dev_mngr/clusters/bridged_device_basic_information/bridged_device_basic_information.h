/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "bridge_dev_mngr_private.h"

namespace bridge::clusters::bridged_device_basic_information {

constexpr uint32_t kClusterId = UINT32_C(0x0039);
constexpr uint32_t kNodeLabelAttributeId = UINT32_C(0x0005);
constexpr uint32_t kReachableAttributeId = UINT32_C(0x0011);

esp_err_t create(esp_matter::endpoint_t *endpoint, device_record &device);
esp_err_t set_name(uint16_t endpoint_id, const char *name);
esp_err_t set_reachable(uint16_t endpoint_id, bool reachable);
extern const cluster_adapter adapter;

} // namespace bridge::clusters::bridged_device_basic_information
