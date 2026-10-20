/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Seed a missing DATA rules file from SYSTEM. Preserve every existing file. */
esp_err_t matter_bridge_seed_router_rules(const char *path, const char *defaults,
                                          bool *seeded);
