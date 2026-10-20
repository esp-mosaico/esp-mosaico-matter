/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "bridge_core.h"
#include "bridge_dev_mngr.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Inject the application-owned bridge handles before starting the UI. */
void mosaic_matter_bridge_set_handles(bridge_core_handle_t core,
                                      bridge_dev_mngr_handle_t dev_mngr);

#ifdef __cplusplus
}
#endif
