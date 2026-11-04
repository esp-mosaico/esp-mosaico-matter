/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "bridge_core.h"
#include "bridge_dev_mngr.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bind borrowed, matching handles before HTTP starts; (NULL, NULL) unbinds.
 * Handles must remain alive until HTTP stops. Changes while serving are rejected. */
esp_err_t http_server_matter_set_bridge(bridge_core_handle_t core,
                                       bridge_dev_mngr_handle_t manager);

#ifdef __cplusplus
}
#endif
