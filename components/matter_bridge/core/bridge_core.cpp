/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "bridge_core_private.h"
#include "bridge_dev_mngr_private.h"

#include <new>

#include "esp_matter_bridge.h"
#include <setup_payload/OnboardingCodesUtil.h>

using namespace esp_matter;

namespace {

bridge_core_handle_t s_core;

esp_err_t attribute_changed(attribute::callback_type_t phase,
                            uint16_t endpoint_id, uint32_t cluster_id,
                            uint32_t attribute_id,
                            esp_matter_attr_val_t *value, void *context)
{
    (void)context;
    /* ESP-Matter supplies the updated endpoint's private data here. Dynamic
     * bridge endpoints store their device_record in that slot, so it cannot
     * be used as the core context. This component supports one core instance;
     * route the global attribute callback through that owner instead. */
    auto *core = s_core;
    if (!core || !core->dev_mngr) {
        return ESP_OK;
    }
    return bridge_dev_mngr_attribute_callback(core->dev_mngr, phase, endpoint_id,
                                       cluster_id, attribute_id, value);
}

} // namespace

esp_err_t bridge_core_create(bridge_core_handle_t *out_core)
{
    if (!out_core || s_core) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_core = nullptr;
    auto *core = new (std::nothrow) bridge_core{};
    if (!core) {
        return ESP_ERR_NO_MEM;
    }
    node::config_t node_config;
    core->node = node::create(&node_config, attribute_changed, nullptr);
    if (!core->node) {
        delete core;
        return ESP_FAIL;
    }
    endpoint::aggregator::config_t aggregator_config;
    auto *aggregator = endpoint::aggregator::create(
        core->node, &aggregator_config, ENDPOINT_FLAG_NONE, nullptr);
    if (!aggregator) {
        node::destroy_raw();
        delete core;
        return ESP_FAIL;
    }
    core->aggregator_endpoint_id = endpoint::get_id(aggregator);
    s_core = core;
    *out_core = core;
    return ESP_OK;
}

esp_err_t bridge_core_delete(bridge_core_handle_t core)
{
    if (!core || core != s_core || core->started || core->dev_mngr) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = node::destroy_raw();
    if (error != ESP_OK) {
        return error;
    }
    s_core = nullptr;
    delete core;
    return ESP_OK;
}

esp_err_t bridge_core_attach_dev_mngr(bridge_core_handle_t core, bridge_dev_mngr *dev_mngr)
{
    if (!core || core != s_core || !dev_mngr || core->started || core->dev_mngr) {
        return ESP_ERR_INVALID_STATE;
    }
    core->dev_mngr = dev_mngr;
    return ESP_OK;
}

esp_err_t bridge_core_detach_dev_mngr(bridge_core_handle_t core, bridge_dev_mngr *dev_mngr)
{
    if (!core || core != s_core || core->started || core->dev_mngr != dev_mngr) {
        return ESP_ERR_INVALID_STATE;
    }
    core->dev_mngr = nullptr;
    return ESP_OK;
}

bool bridge_core_is_running(bridge_core_handle_t core)
{
    return core && core == s_core && core->started;
}

esp_err_t bridge_core_start(bridge_core_handle_t core)
{
    if (!core || core != s_core || core->started || !core->dev_mngr) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = bridge_dev_mngr_prepare_start(core->dev_mngr);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_matter::start(nullptr);
    if (error == ESP_OK) {
        core->started = true;
        PrintOnboardingCodes(chip::RendezvousInformationFlag::kBLE);
    }
    return error;
}
