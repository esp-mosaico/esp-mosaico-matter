/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_core_private.h"
#include "fabric_core_attestation_verifier.h"

#include "esp_check.h"
#include "esp_matter.h"
#include "esp_matter_controller_client.h"
#include "esp_matter_da_revocation_delegate.h"

#include <controller/CHIPDeviceController.h>
#include <controller/CommissioningDelegate.h>
#include <lib/core/DataModelTypes.h>

#include <cstring>
#include <optional>

namespace {

constexpr size_t kWifiSsidCapacity = 33;
constexpr size_t kWifiPasswordCapacity = 65;
constexpr size_t kThreadDatasetCapacity = 254;
constexpr uint16_t kControllerListenPort = 5580;
const char *const kTag = "fabric_core_chip";

enum class commissioning_phase : uint8_t {
    idle,
    preparing,
    pairing,
    canceling,
};

struct commissioning_context {
    commissioning_phase phase = commissioning_phase::idle;
    fabric_core_handle_t core = nullptr;
    uint64_t node_id = 0;
    fabric_core_request_id_t request_id = 0;
    fabric_core_request_completion_cb_t completion_cb = nullptr;
    void *user_ctx = nullptr;

    char wifi_ssid[kWifiSsidCapacity] = {};
    char wifi_password[kWifiPasswordCapacity] = {};
    uint8_t thread_dataset[kThreadDatasetCapacity] = {};
    size_t thread_dataset_size = 0;
};

commissioning_context s_commissioning;
bool s_fabric_core_chip_initialized;

uint64_t fabric_core_next_commissioned_node_id(fabric_core_handle_t core)
{
    uint64_t candidate = core->next_node_id;
    while (candidate != UINT64_MAX) {
        core->next_node_id = candidate + 1;
        if (chip::IsOperationalNodeId(candidate) &&
            candidate != core->controller_node_id &&
            fabric_core_find_node(core, candidate) == nullptr) {
            return candidate;
        }
        candidate = core->next_node_id;
    }
    return chip::kUndefinedNodeId;
}

void fabric_core_finish_commissioning(CHIP_ERROR chip_error)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    commissioning_context context = s_commissioning;
    fabric_core_handle_t core = context.core;
    if (core == nullptr) {
        return;
    }

    auto &commissioner = *esp_matter::controller::matter_controller_client::
        get_instance().get_commissioner();
    commissioner.RegisterPairingDelegate(nullptr);

    esp_err_t error = fabric_core_from_chip_error(chip_error);
    fabric_core_node *node = nullptr;
    if (error == ESP_OK) {
        if (core->node_count >= core->config.max_nodes) {
            error = ESP_ERR_NO_MEM;
        } else if (fabric_core_find_node(core, context.node_id) != nullptr) {
            error = ESP_ERR_INVALID_STATE;
        } else {
            node = &core->nodes[core->node_count++];
            node->node_id = context.node_id;
            node->subscription_state = FABRIC_CORE_SUBSCRIPTION_TERMINATED;
        }

        if (error == ESP_OK) {
            error = fabric_core_store_nodes(core);
            if (error == ESP_OK) {
                fabric_core_private_notify_node_set_changed(core);
            }
        }
        if (error != ESP_OK && node != nullptr) {
            memset(node, 0, sizeof(*node));
            --core->node_count;
            node = nullptr;
        }
    }

    bool start_subscription = error == ESP_OK && node != nullptr &&
                              core->lifecycle == FABRIC_CORE_RUNNING;
    s_commissioning = {};

    if (start_subscription) {
        esp_err_t subscription_error = fabric_core_subscription_start_node(core, node);
        if (subscription_error != ESP_OK) {
            // Commissioning succeeded and remains persisted. Subscription state
            // reports the independent connectivity failure.
        }
    }

    fabric_core_request_result_t result = {
        .request_id = context.request_id,
        .operation = FABRIC_CORE_OPERATION_COMMISSION,
        .error = error,
        .node_id = context.node_id,
        .value = {},
    };
    context.completion_cb(core, &result, context.user_ctx);
    fabric_core_request_end(core);
}

void fabric_core_abandon_commissioning(fabric_core_handle_t core)
{
    s_commissioning = {};
    fabric_core_request_end(core);
}

class commissioning_delegate final
    : public chip::Controller::DevicePairingDelegate {
public:
    void OnPairingComplete(CHIP_ERROR error) override
    {
        if (error != CHIP_NO_ERROR) {
            fabric_core_finish_commissioning(error);
        }
    }

    void OnCommissioningSuccess(chip::PeerId peer_id) override
    {
        if (s_commissioning.core != nullptr &&
            peer_id.GetNodeId() == s_commissioning.node_id) {
            fabric_core_finish_commissioning(CHIP_NO_ERROR);
        }
    }

    void OnCommissioningFailure(
        chip::PeerId peer_id, CHIP_ERROR error,
        chip::Controller::CommissioningStage stage,
        chip::Optional<chip::Credentials::AttestationVerificationResult>
            additional_error) override
    {
        (void)peer_id;
        (void)stage;
        (void)additional_error;
        fabric_core_finish_commissioning(error);
    }

    void OnICDRegistrationComplete(chip::ScopedNodeId node_id,
                                   uint32_t icd_counter) override
    {
        (void)node_id;
        (void)icd_counter;
    }

    void OnICDStayActiveComplete(chip::ScopedNodeId node_id,
                                 uint32_t promised_active_duration) override
    {
        (void)node_id;
        (void)promised_active_duration;
    }
};

commissioning_delegate s_commissioning_delegate;

esp_err_t reserve_commissioned_node(fabric_core_handle_t core,
                                    commissioning_context &prepared)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (core->lifecycle != FABRIC_CORE_RUNNING ||
        s_commissioning.phase != commissioning_phase::idle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (core->node_count >= core->config.max_nodes) {
        return ESP_ERR_NO_MEM;
    }
    prepared.node_id = fabric_core_next_commissioned_node_id(core);
    if (prepared.node_id == chip::kUndefinedNodeId) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(fabric_core_store_nodes(core), kTag,
                        "store next commissioned node id");
    s_commissioning = prepared;
    return ESP_OK;
}

bool commissioning_cancelled(fabric_core_handle_t core)
{
    return s_commissioning.phase == commissioning_phase::canceling ||
           core->lifecycle != FABRIC_CORE_RUNNING;
}

esp_err_t load_wifi_credentials(fabric_core_handle_t core,
                                commissioning_context &prepared)
{
    if (!core->config.get_wifi_credentials) {
        return ESP_OK;
    }
    return core->config.get_wifi_credentials(
        prepared.wifi_ssid, sizeof(prepared.wifi_ssid),
        prepared.wifi_password, sizeof(prepared.wifi_password),
        core->config.wifi_credentials_ctx);
}

esp_err_t load_thread_dataset(fabric_core_handle_t core,
                              commissioning_context &prepared)
{
    if (!core->config.get_thread_dataset) {
        return ESP_OK;
    }
    esp_err_t error = core->config.get_thread_dataset(
        prepared.thread_dataset, sizeof(prepared.thread_dataset),
        &prepared.thread_dataset_size, core->config.thread_dataset_ctx);
    if (error == ESP_ERR_NOT_FOUND) {
        prepared.thread_dataset_size = 0;
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    return prepared.thread_dataset_size <= sizeof(prepared.thread_dataset)
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

void set_commissioning_credentials(
    fabric_core_handle_t core, const commissioning_context &prepared,
    chip::Controller::CommissioningParameters &parameters)
{
    if (core->config.get_wifi_credentials) {
        parameters.SetWiFiCredentials(chip::Controller::WiFiCredentials(
            chip::ByteSpan(reinterpret_cast<const uint8_t *>(
                               prepared.wifi_ssid),
                           strlen(prepared.wifi_ssid)),
            chip::ByteSpan(reinterpret_cast<const uint8_t *>(
                               prepared.wifi_password),
                           strlen(prepared.wifi_password))));
    }
    if (prepared.thread_dataset_size > 0) {
        parameters.SetThreadOperationalDataset(chip::ByteSpan(
            prepared.thread_dataset, prepared.thread_dataset_size));
    }
}

} // namespace

esp_err_t fabric_core_from_chip_error(CHIP_ERROR error)
{
    if (error == CHIP_NO_ERROR) {
        return ESP_OK;
    }
    if (error == CHIP_ERROR_NO_MEMORY) {
        return ESP_ERR_NO_MEM;
    }
    if (error == CHIP_ERROR_INVALID_ARGUMENT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (error == CHIP_ERROR_INCORRECT_STATE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (error == CHIP_ERROR_TIMEOUT) {
        return ESP_ERR_TIMEOUT;
    }
    if (error == CHIP_ERROR_CANCELLED) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_FAIL;
}

esp_err_t fabric_core_chip_start(fabric_core_handle_t core)
{
    if (s_fabric_core_chip_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_matter::start(nullptr), kTag, "start Matter stack");

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto &client = esp_matter::controller::matter_controller_client::get_instance();
    ESP_RETURN_ON_ERROR(
        client.init(core->controller_node_id, core->fabric_id,
                    kControllerListenPort),
        kTag, "initialize controller");
    ESP_RETURN_ON_ERROR(client.setup_commissioner(), kTag,
                        "setup commissioner");
    if (core->config.skip_paa_and_cd_checks) {
        static local_fabric::paa_cd_bypass_verifier verifier(
            chip::Credentials::get_da_revocation_delegate());
        chip::Credentials::SetDeviceAttestationVerifier(&verifier);
        client.get_commissioner()->SetDeviceAttestationVerifier(&verifier);
        ESP_LOGW(kTag, "Commissioning bypasses PAA trust-chain and CD verification");
    }
    s_fabric_core_chip_initialized = true;
    return ESP_OK;
}

void fabric_core_chip_stop(fabric_core_handle_t core)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (s_commissioning.core != core) {
        return;
    }

    if (s_commissioning.phase == commissioning_phase::preparing) {
        s_commissioning.phase = commissioning_phase::canceling;
        return;
    }
    if (s_commissioning.phase != commissioning_phase::pairing) {
        return;
    }

    auto *commissioner = esp_matter::controller::matter_controller_client::
        get_instance().get_commissioner();
    CHIP_ERROR error = commissioner->StopPairing(s_commissioning.node_id);
    if (error != CHIP_NO_ERROR) {
        fabric_core_finish_commissioning(error);
    }
    // ESP-Matter 1.6 has no controller-stack stop API. The process-wide stack
    // remains initialized after Local Fabric subscriptions are detached.
}

esp_err_t fabric_core_chip_start_commissioning(
    fabric_core_handle_t core, const char *setup_code,
    fabric_core_request_id_t request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    esp_err_t error = fabric_core_request_begin(core);
    if (error != ESP_OK) {
        return error;
    }

    commissioning_context prepared = {
        .phase = commissioning_phase::preparing,
        .core = core,
        .node_id = 0,
        .request_id = request_id,
        .completion_cb = completion_cb,
        .user_ctx = user_ctx,
    };
    error = reserve_commissioned_node(core, prepared);
    if (error != ESP_OK) {
        fabric_core_request_end(core);
        return error;
    }

    {
        esp_matter::lock::ScopedChipStackLock callback_lock(portMAX_DELAY);
        if (commissioning_cancelled(core)) {
            fabric_core_finish_commissioning(CHIP_ERROR_CANCELLED);
            return ESP_OK;
        }
        error = load_wifi_credentials(core, prepared);
        if (commissioning_cancelled(core)) {
            fabric_core_finish_commissioning(CHIP_ERROR_CANCELLED);
            return ESP_OK;
        }
        if (error == ESP_OK) {
            error = load_thread_dataset(core, prepared);
        }
        if (commissioning_cancelled(core)) {
            fabric_core_finish_commissioning(CHIP_ERROR_CANCELLED);
            return ESP_OK;
        }
        if (error != ESP_OK) {
            fabric_core_abandon_commissioning(core);
            return error;
        }
    }

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (commissioning_cancelled(core)) {
        fabric_core_finish_commissioning(CHIP_ERROR_CANCELLED);
        return ESP_OK;
    }

    prepared.phase = commissioning_phase::pairing;
    s_commissioning = prepared;
    auto *commissioner = esp_matter::controller::matter_controller_client::
        get_instance().get_commissioner();
    commissioner->RegisterPairingDelegate(&s_commissioning_delegate);

    chip::Controller::CommissioningParameters parameters;
    set_commissioning_credentials(core, s_commissioning, parameters);
    const CHIP_ERROR chip_error = commissioner->PairDevice(
        prepared.node_id, setup_code, parameters,
        chip::Controller::DiscoveryType::kAll);
    if (chip_error != CHIP_NO_ERROR) {
        commissioner->RegisterPairingDelegate(nullptr);
        fabric_core_abandon_commissioning(core);
    }
    return fabric_core_from_chip_error(chip_error);
}

esp_err_t fabric_core_chip_cancel_commissioning(fabric_core_handle_t core)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (s_commissioning.core != core) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_commissioning.phase == commissioning_phase::preparing) {
        s_commissioning.phase = commissioning_phase::canceling;
        return ESP_OK;
    }
    if (s_commissioning.phase != commissioning_phase::pairing) {
        return ESP_ERR_INVALID_STATE;
    }

    auto *commissioner = esp_matter::controller::matter_controller_client::
        get_instance().get_commissioner();
    CHIP_ERROR error = commissioner->StopPairing(s_commissioning.node_id);
    if (error == CHIP_NO_ERROR ||
        error == CHIP_ERROR_INVALID_DEVICE_DESCRIPTOR) {
        // StopPairing does not notify DevicePairingDelegate while setup-code
        // discovery is in progress. Complete the Local Fabric request here;
        // if CHIP already delivered a synchronous callback this is a no-op.
        fabric_core_finish_commissioning(CHIP_ERROR_CANCELLED);
        return ESP_OK;
    }
    return fabric_core_from_chip_error(error);
}
