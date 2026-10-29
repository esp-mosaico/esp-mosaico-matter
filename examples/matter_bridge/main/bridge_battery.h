/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "mosaic_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Sample immediately and publish changes every five seconds. Call after UI startup. */
esp_err_t bridge_battery_start(void);

/** Read the cache without I2C access; unavailable until the first valid sample. */
esp_err_t bridge_battery_get(mosaic_settings_battery_t *out);

#ifdef __cplusplus
}
#endif
