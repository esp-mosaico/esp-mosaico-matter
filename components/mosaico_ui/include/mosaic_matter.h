/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "fabric_core.h"
#include "fabric_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Supplies the Local Fabric handles used by the controller-facing Matter app. */
void mosaic_matter_set_local_fabric(fabric_core_handle_t core,
                                    fabric_manager_handle_t manager);

#ifdef __cplusplus
}
#endif
