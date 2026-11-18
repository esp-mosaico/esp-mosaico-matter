/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "model_path.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*hub_wake_cb_t)(const char *phrase, void *ctx);

/**
 * Start WakeNet on a core-1 task with a PSRAM stack.
 * `models` comes from esp_srmodel_init() and must already be mapped.
 * The callback runs on the WakeNet task and must only signal another task.
 */
esp_err_t hub_wake_start(srmodel_list_t *models, hub_wake_cb_t cb, void *ctx);

/** Feed 16 kHz mono PCM. Safe to call before start; those samples are ignored. */
void hub_wake_feed(const int16_t *pcm, size_t samples);

/**
 * Wake phrase owned by WakeNet. Empty until the model is ready.
 */
const char *hub_wake_phrase(void);

#ifdef __cplusplus
}
#endif
