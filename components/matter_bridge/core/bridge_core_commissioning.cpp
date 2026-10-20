/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "bridge_core_private.h"
#include "bridge_dev_mngr_private.h"

#include <cstring>

#include "esp_matter_bridge.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>

namespace {

using payload_buffer =
    char[chip::QRCodeBasicSetupPayloadGenerator::
             kMaxQRCodeBase38RepresentationLength +
         1];

bool on_network_ready()
{
    wifi_ap_record_t ap{};
    auto *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info{};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK && netif &&
        esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr;
}

esp_err_t render_onboarding_code(
    bridge_core_handle_t core,
    CHIP_ERROR (*generate)(chip::MutableCharSpan &,
                           chip::RendezvousInformationFlags),
    chip::MutableCharSpan &span, char *out_text, size_t out_text_len)
{
    if (!bridge_core_is_running(core)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!out_text || !out_text_len) {
        return ESP_ERR_INVALID_ARG;
    }
    out_text[0] = '\0';
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    CHIP_ERROR error = generate(
        span, chip::RendezvousInformationFlags(
                  on_network_ready() ? chip::RendezvousInformationFlag::kOnNetwork
                                     : chip::RendezvousInformationFlag::kBLE));
    if (error != CHIP_NO_ERROR) {
        return ESP_FAIL;
    }
    const size_t length = strlen(span.data());
    if (length >= out_text_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out_text, span.data(), length + 1);
    return ESP_OK;
}

} // namespace

esp_err_t bridge_core_open_commissioning_window(bridge_core_handle_t core)
{
    if (!bridge_core_is_running(core)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto &server = chip::Server::GetInstance();
    if (!server.GetFailSafeContext().IsFailSafeFullyDisarmed()) {
        return ESP_ERR_INVALID_STATE;
    }
    auto &window = server.GetCommissioningWindowManager();
    if (window.IsCommissioningWindowOpen()) window.CloseCommissioningWindow();
    CHIP_ERROR error = window.OpenBasicCommissioningWindow(
        chip::System::Clock::Seconds16(180),
        chip::CommissioningWindowAdvertisement::kAllSupported);
    if (error != CHIP_NO_ERROR) {
        return ESP_FAIL;
    }
    PrintOnboardingCodes(chip::RendezvousInformationFlag::kBLE);
    return ESP_OK;
}

esp_err_t bridge_core_open_on_network_commissioning_window(bridge_core_handle_t core)
{
    if (!bridge_core_is_running(core)) return ESP_ERR_INVALID_STATE;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto &server = chip::Server::GetInstance();
    if (!on_network_ready() || server.GetFabricTable().FabricCount() != 0 ||
            !server.GetFailSafeContext().IsFailSafeFullyDisarmed()) return ESP_ERR_INVALID_STATE;
    auto &window = server.GetCommissioningWindowManager();
    if (window.IsCommissioningWindowOpen()) window.CloseCommissioningWindow();
    CHIP_ERROR error = window.OpenBasicCommissioningWindow(
        chip::System::Clock::Seconds16(180), chip::CommissioningWindowAdvertisement::kDnssdOnly);
    if (error != CHIP_NO_ERROR) return ESP_FAIL;
    PrintOnboardingCodes(chip::RendezvousInformationFlag::kOnNetwork);
    return ESP_OK;
}

esp_err_t bridge_core_get_qr_code(bridge_core_handle_t core, char *out_text,
                               size_t out_text_len)
{
    static payload_buffer payload{};
    chip::MutableCharSpan span(payload);
    return render_onboarding_code(core, GetQRCode, span, out_text,
                                  out_text_len);
}

esp_err_t bridge_core_get_manual_pairing_code(bridge_core_handle_t core,
                                            char *out_text,
                                            size_t out_text_len)
{
    static payload_buffer payload{};
    chip::MutableCharSpan span(payload);
    return render_onboarding_code(core, GetManualPairingCode, span, out_text,
                                  out_text_len);
}

esp_err_t bridge_core_is_commissioned(bridge_core_handle_t core,
                                   bool *out_commissioned)
{
    if (!bridge_core_is_running(core)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!out_commissioned) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    *out_commissioned =
        chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
    return ESP_OK;
}

esp_err_t bridge_core_factory_reset(bridge_core_handle_t core)
{
    if (!bridge_core_is_running(core) || !core->dev_mngr) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = bridge_dev_mngr_factory_reset(core->dev_mngr);
    if (error == ESP_OK) {
        error = esp_matter_bridge::factory_reset();
    }
    if (error == ESP_OK) {
        error = esp_matter::factory_reset();
    }
    return error;
}
