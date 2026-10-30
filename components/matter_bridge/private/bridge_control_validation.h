/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstring>
#include "bridge_dev_mngr.h"

namespace bridge {

inline esp_err_t validate_control(const char *profile,
                                  const bridge_dev_mngr_attribute_path_t &path,
                                  const bridge_dev_mngr_value_t &value)
{
    const bool color = !strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT);
    const bool dimmable = color || !strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_DIMMABLE_LIGHT);
    const bool power = dimmable || !strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_ON_OFF_LIGHT) ||
                       !strcmp(profile, BRIDGE_DEV_MNGR_PROFILE_OUTLET);
    if (value.truncated) return ESP_ERR_INVALID_ARG;
    if (power && path.cluster_id == 0x0006 && path.attribute_id == 0) {
        return value.type == BRIDGE_DEV_MNGR_VALUE_BOOL ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (dimmable && path.cluster_id == 0x0008 && path.attribute_id == 0) {
        return value.type == BRIDGE_DEV_MNGR_VALUE_UNSIGNED &&
               value.data.unsigned_value >= 1 && value.data.unsigned_value <= 254
                   ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (color && path.cluster_id == 0x0300) {
        if (path.attribute_id == 0 || path.attribute_id == 1) {
            return value.type == BRIDGE_DEV_MNGR_VALUE_UNSIGNED &&
                   value.data.unsigned_value <= 254 ? ESP_OK : ESP_ERR_INVALID_ARG;
        }
        if (path.attribute_id == 7) {
            return value.type == BRIDGE_DEV_MNGR_VALUE_UNSIGNED &&
                   value.data.unsigned_value > 0 && value.data.unsigned_value < 65535
                       ? ESP_OK : ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_ERR_NOT_SUPPORTED;
}

} // namespace bridge
