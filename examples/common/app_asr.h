/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bind ASR or an unavailable status to AI Create after audio_hub starts.
 * Pass NULL for previous at startup or if the old config could not be read.
 * The caller serializes startup, config writes and reloads. */
esp_err_t app_asr_reload(const app_config_t *previous, const app_config_t *config);

#ifdef __cplusplus
}
#endif
