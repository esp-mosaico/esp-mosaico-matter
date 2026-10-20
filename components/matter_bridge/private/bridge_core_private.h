/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "bridge_core.h"

#include "esp_matter.h"

struct bridge_dev_mngr;

struct bridge_core {
    esp_matter::node_t *node;
    uint16_t aggregator_endpoint_id;
    bool started;
    struct bridge_dev_mngr *dev_mngr;
};

esp_err_t bridge_core_attach_dev_mngr(bridge_core_handle_t core, struct bridge_dev_mngr *dev_mngr);
esp_err_t bridge_core_detach_dev_mngr(bridge_core_handle_t core, struct bridge_dev_mngr *dev_mngr);
bool bridge_core_is_running(bridge_core_handle_t core);
