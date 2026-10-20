/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "fabric_core.h"
#include "fabric_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Binds a matching Local Fabric core/device-manager pair, or unbinds with
 * (NULL, NULL). A partial pair returns ESP_ERR_INVALID_ARG. Repeating the
 * current binding is a no-op. Overlapping binding changes or pending HTTP
 * Matter operations return ESP_ERR_INVALID_STATE.
 *
 * A failed old-listener removal preserves the old binding. If new-listener
 * registration fails after removal, restoration of the old binding is tried;
 * if restoration also fails, routes remain unbound and the failure is logged.
 * Registration errors are returned to the caller; no listener IDs are lost.
 *
 * The binding borrows both handles: keep them alive while bound and until
 * this call completes. Quiesce HTTP requests and outstanding Matter operations
 * before replacing or unbinding. Do not call while holding resources needed
 * by Manager callbacks. Routes are unavailable during a binding change, and SSE
 * clients resynchronize when it finishes.
 */
esp_err_t http_server_matter_set_local_fabric(fabric_core_handle_t core,
                                            fabric_manager_handle_t manager);

/** True only for a completed binding with a registered event listener. */
bool http_server_matter_enabled(void);

#ifdef __cplusplus
}
#endif
