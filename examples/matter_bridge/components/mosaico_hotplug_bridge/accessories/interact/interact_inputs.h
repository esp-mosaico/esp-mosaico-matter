/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_err.h"
#include "mosaico_module_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

void mosaico_interact_inputs_init(void);
esp_err_t mosaico_interact_inputs_open(mosaico_module_mgr_slot_t slot);
esp_err_t mosaico_interact_inputs_close(mosaico_module_mgr_slot_t slot);
esp_err_t mosaico_interact_pir_read(mosaico_module_mgr_slot_t slot, bool *occupied);
// Filtered nominal ADC voltage, retaining fractional mV for the LDR model.
esp_err_t mosaico_interact_light_read(mosaico_module_mgr_slot_t slot, double *millivolts);

#ifdef __cplusplus
}
#endif
