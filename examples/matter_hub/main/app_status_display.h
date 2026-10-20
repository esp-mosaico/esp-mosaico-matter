/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"

/** Start the non-GSP hub status screen. Widgets/timer live for the application. */
esp_err_t app_status_display_start(void);
