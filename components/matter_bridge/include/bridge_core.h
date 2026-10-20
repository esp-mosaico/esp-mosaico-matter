/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque owner of the single Matter bridge server in one firmware image. */
typedef struct bridge_core *bridge_core_handle_t;

/** Creates a stopped Matter bridge core. Only one core may exist. */
esp_err_t bridge_core_create(bridge_core_handle_t *out_core);

/**
 * Deletes a core which has not started and has no attached device manager.
 * ESP-Matter does not expose a supported server stop path, so a successfully
 * started core lives until restart or factory reset.
 */
esp_err_t bridge_core_delete(bridge_core_handle_t core);

/** Reconciles the attached device catalog and starts the Matter server. */
esp_err_t bridge_core_start(bridge_core_handle_t core);

/** Opens or reopens the standard 180-second BLE commissioning window.
 * Does not interrupt an armed fail-safe. */
esp_err_t bridge_core_open_commissioning_window(bridge_core_handle_t core);

/** Opens a 180-second DNS-SD-only window for an uncommissioned Wi-Fi bridge.
 * Requires an active STA connection and does not interrupt an armed fail-safe. */
esp_err_t bridge_core_open_on_network_commissioning_window(bridge_core_handle_t core);

/** Renders the current QR onboarding payload into caller-owned storage.
 * Selects on-network discovery when STA has an IP, otherwise BLE. */
esp_err_t bridge_core_get_qr_code(bridge_core_handle_t core, char *out_text,
                                  size_t out_text_len);

/** Renders the current manual pairing code into caller-owned storage. */
esp_err_t bridge_core_get_manual_pairing_code(bridge_core_handle_t core,
                                               char *out_text,
                                               size_t out_text_len);

/** Returns whether at least one Matter fabric is installed. */
esp_err_t bridge_core_is_commissioned(bridge_core_handle_t core,
                                      bool *out_commissioned);

/** Erases bridge/catalog state and schedules the platform Matter reset. */
esp_err_t bridge_core_factory_reset(bridge_core_handle_t core);

#ifdef __cplusplus
}
#endif
