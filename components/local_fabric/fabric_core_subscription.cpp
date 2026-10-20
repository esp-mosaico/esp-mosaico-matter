/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_core_private.h"

#include "cJSON.h"
#include "esp_matter.h"
#include "esp_matter_controller_client.h"
#include "tlv_to_json.h"

#include <app/InteractionModelEngine.h>
#include <app/ReadClient.h>
#include <app/ReadPrepareParams.h>
#include <lib/core/TLV.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/PlatformManager.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace
{

using chip::app::AttributePathParams;
using chip::app::ConcreteDataAttributePath;
using chip::app::InteractionModelEngine;
using chip::app::ReadClient;
using chip::app::ReadPrepareParams;
using chip::app::StatusIB;

struct fabric_core_subscription final : ReadClient::Callback {
    fabric_core_handle_t core;
    fabric_core_node *node;
    AttributePathParams wildcard_path;
    ReadClient *client = nullptr;
    bool established = false;
    bool priming_report_complete = false;
    bool report_open = false;
    CHIP_ERROR report_error = CHIP_NO_ERROR;

    fabric_core_subscription(fabric_core_handle_t core_handle, fabric_core_node *node_entry)
        : core(core_handle), node(node_entry), wildcard_path()
    {
    }

    CHIP_ERROR start()
    {
        if (client != nullptr) {
            return CHIP_ERROR_INCORRECT_STATE;
        }
        established = false;
        priming_report_complete = false;
        report_open = false;
        report_error = CHIP_NO_ERROR;
        wildcard_path = AttributePathParams();
        client = chip::Platform::New<ReadClient>(
            InteractionModelEngine::GetInstance(), nullptr, *this,
            ReadClient::InteractionType::Subscribe);
        VerifyOrReturnError(client != nullptr, CHIP_ERROR_NO_MEMORY);

        ReadPrepareParams params;
        params.mpAttributePathParamsList = &wildcard_path;
        params.mAttributePathParamsListSize = 1;
        params.mMinIntervalFloorSeconds =
            core->config.subscription_interval_s_min;
        params.mMaxIntervalCeilingSeconds =
            core->config.subscription_interval_s_max;
        params.mKeepSubscriptions = true;
        const chip::ScopedNodeId peer(
            node->node_id,
            esp_matter::controller::matter_controller_client::get_instance()
                .get_fabric_index());
        CHIP_ERROR error =
            client->SendAutoResubscribeRequest(peer, std::move(params));
        if (error != CHIP_NO_ERROR) {
            // SendAutoResubscribeRequest has already returned retained paths.
            chip::Platform::Delete(client);
            client = nullptr;
        }
        return error;
    }

    void stop()
    {
        if (client != nullptr) {
            chip::Platform::Delete(client);
            client = nullptr;
        }
        established = false;
        priming_report_complete = false;
        report_open = false;
    }

    void OnReportBegin() override;
    void OnReportEnd() override;
    void OnAttributeData(const ConcreteDataAttributePath &path,
                         chip::TLV::TLVReader *data,
                         const StatusIB &status) override;
    void
    OnSubscriptionEstablished(chip::SubscriptionId subscription_id) override;
    CHIP_ERROR OnResubscriptionNeeded(ReadClient *read_client,
                                      CHIP_ERROR cause) override;
    void OnError(CHIP_ERROR error) override;
    void OnDone(ReadClient *read_client) override;
    void OnDeallocatePaths(ReadPrepareParams &&params) override
    {
        (void)params;
    }
};

void fabric_core_dispatch_listeners(fabric_core_handle_t core,
                                void (*dispatch)(const fabric_core_listener_t &,
                                                 void *),
                                void *argument)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (core->lifecycle != FABRIC_CORE_RUNNING) {
        return;
    }
    for (const auto &entry : core->listeners) {
        if (entry.id != 0) {
            dispatch(entry.listener, argument);
        }
    }
}

template <typename Callback>
void fabric_core_dispatch_private(fabric_core_handle_t core, Callback callback)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (core->lifecycle == FABRIC_CORE_RUNNING &&
        core->private_listener_installed) {
        callback(core->private_listener);
    }
}

bool fabric_core_has_public_attribute_listener(fabric_core_handle_t core)
{
    bool found = false;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    for (const auto &entry : core->listeners) {
        if (entry.id != 0 && entry.listener.attribute_reported != nullptr) {
            found = true;
            break;
        }
    }
    return found;
}

struct subscription_dispatch {
    fabric_core_handle_t core;
    fabric_core_subscription_update_t update;
};

void fabric_core_dispatch_subscription(const fabric_core_listener_t &listener,
                                   void *argument)
{
    const auto &dispatch =
        *static_cast<const subscription_dispatch *>(argument);
    if (listener.subscription_changed != nullptr) {
        listener.subscription_changed(dispatch.core, &dispatch.update,
                                      listener.user_ctx);
    }
}

void fabric_core_set_subscription_state(fabric_core_subscription &subscription,
                                    fabric_core_subscription_state_t state,
                                    CHIP_ERROR cause = CHIP_NO_ERROR)
{
    fabric_core_handle_t core = subscription.core;
    bool changed = false;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (subscription.node->subscription_state != state) {
        subscription.node->subscription_state = state;
        changed = true;
    }
    if (!changed) {
        return;
    }
    subscription_dispatch dispatch = {
        .core = core,
        .update =
            {
                .node_id = subscription.node->node_id,
                .state = state,
                .cause = fabric_core_from_chip_error(cause),
            },
    };
    fabric_core_dispatch_private(
        core, [&](const fabric_core_private_listener &listener) {
            if (listener.subscription_changed != nullptr) {
                listener.subscription_changed(core, &dispatch.update,
                                              listener.user_ctx);
            }
        });
    fabric_core_dispatch_listeners(core, fabric_core_dispatch_subscription, &dispatch);
}

struct report_dispatch {
    fabric_core_handle_t core;
    uint64_t node_id;
    esp_err_t error;
};

void fabric_core_dispatch_report_begin(const fabric_core_listener_t &listener,
                                   void *argument)
{
    const auto &dispatch = *static_cast<const report_dispatch *>(argument);
    if (listener.report_begin != nullptr) {
        listener.report_begin(dispatch.core, dispatch.node_id,
                              listener.user_ctx);
    }
}

void fabric_core_dispatch_report_end(const fabric_core_listener_t &listener,
                                 void *argument)
{
    const auto &dispatch = *static_cast<const report_dispatch *>(argument);
    if (listener.report_end != nullptr) {
        listener.report_end(dispatch.core, dispatch.node_id, dispatch.error,
                            listener.user_ctx);
    }
}

struct attribute_dispatch {
    fabric_core_handle_t core;
    fabric_core_attribute_report_t report;
};

void fabric_core_dispatch_attribute(const fabric_core_listener_t &listener,
                                void *argument)
{
    const auto &dispatch = *static_cast<const attribute_dispatch *>(argument);
    if (listener.attribute_reported != nullptr) {
        listener.attribute_reported(dispatch.core, &dispatch.report,
                                    listener.user_ctx);
    }
}

void fabric_core_copy_text(fabric_core_value_t &value, const char *data, size_t size)
{
    const size_t copied = std::min(size, sizeof(value.text) - 1);
    memcpy(value.text, data, copied);
    value.text[copied] = '\0';
    value.truncated = copied != size;
}

CHIP_ERROR fabric_core_decode_tlv_value_impl(chip::TLV::TLVReader &reader,
                                         fabric_core_value_t &value)
{
    value = {};
    switch (reader.GetType()) {
    case chip::TLV::kTLVType_Null:
        value.type = FABRIC_CORE_VALUE_NULL;
        return CHIP_NO_ERROR;
    case chip::TLV::kTLVType_Boolean:
        value.type = FABRIC_CORE_VALUE_BOOL;
        return reader.Get(value.data.boolean);
    case chip::TLV::kTLVType_SignedInteger:
        value.type = FABRIC_CORE_VALUE_SIGNED;
        return reader.Get(value.data.signed_value);
    case chip::TLV::kTLVType_UnsignedInteger:
        value.type = FABRIC_CORE_VALUE_UNSIGNED;
        return reader.Get(value.data.unsigned_value);
    case chip::TLV::kTLVType_FloatingPointNumber:
        value.type = FABRIC_CORE_VALUE_FLOAT;
        return reader.Get(value.data.float_value);
    case chip::TLV::kTLVType_UTF8String: {
        chip::CharSpan span;
        ReturnErrorOnFailure(reader.Get(span));
        value.type = FABRIC_CORE_VALUE_STRING;
        fabric_core_copy_text(value, span.data(), span.size());
        return CHIP_NO_ERROR;
    }
    case chip::TLV::kTLVType_ByteString: {
        chip::ByteSpan span;
        ReturnErrorOnFailure(reader.Get(span));
        value.type = FABRIC_CORE_VALUE_OCTETS;
        static constexpr char kHex[] = "0123456789abcdef";
        const size_t bytes =
            std::min(span.size(), (sizeof(value.text) - 1) / 2);
        for (size_t index = 0; index < bytes; ++index) {
            value.text[index * 2] = kHex[span[index] >> 4];
            value.text[index * 2 + 1] = kHex[span[index] & 0x0f];
        }
        value.text[bytes * 2] = '\0';
        value.truncated = bytes != span.size();
        return CHIP_NO_ERROR;
    }
    default: {
        chip::TLV::TLVReader copy;
        copy.Init(reader);
        cJSON *json = nullptr;
        esp_err_t error = esp_matter::tlv_to_json(
            copy, &json,
            esp_matter::tlv_to_json_options{.human_readable_bytes = true});
        VerifyOrReturnError(error == ESP_OK && json != nullptr,
                            CHIP_ERROR_INVALID_TLV_ELEMENT);
        char *text = cJSON_PrintUnformatted(json);
        cJSON_Delete(json);
        VerifyOrReturnError(text != nullptr, CHIP_ERROR_NO_MEMORY);
        value.type = FABRIC_CORE_VALUE_JSON;
        fabric_core_copy_text(value, text, strlen(text));
        cJSON_free(text);
        return CHIP_NO_ERROR;
    }
    }
}

void fabric_core_subscription::OnReportBegin()
{
    // A priming subscription report can span several ReportData messages.
    // Keep one transaction open until the SubscribeResponse confirms that the
    // complete snapshot has arrived.
    if (report_open) {
        return;
    }
    report_open = true;
    report_error = CHIP_NO_ERROR;
    report_dispatch dispatch = {
        .core = core, .node_id = node->node_id, .error = ESP_OK};
    fabric_core_dispatch_private(
        core, [&](const fabric_core_private_listener &listener) {
            if (listener.report_begin != nullptr) {
                listener.report_begin(core, node->node_id, listener.user_ctx);
            }
        });
    fabric_core_dispatch_listeners(core, fabric_core_dispatch_report_begin, &dispatch);
}

void fabric_core_subscription::OnReportEnd()
{
    if (!report_open) {
        return;
    }
    if (!established && !priming_report_complete) {
        return;
    }
    report_open = false;
    report_dispatch dispatch = {
        .core = core,
        .node_id = node->node_id,
        .error = fabric_core_from_chip_error(report_error),
    };
    fabric_core_dispatch_private(
        core, [&](const fabric_core_private_listener &listener) {
            if (listener.report_end != nullptr) {
                listener.report_end(core, node->node_id, dispatch.error,
                                    !priming_report_complete,
                                    listener.user_ctx);
            }
        });
    fabric_core_dispatch_listeners(core, fabric_core_dispatch_report_end, &dispatch);
    if (report_error == CHIP_NO_ERROR) {
        priming_report_complete = true;
        if (established) {
            fabric_core_set_subscription_state(*this, FABRIC_CORE_SUBSCRIPTION_ACTIVE);
        }
    }
}

void fabric_core_subscription::OnAttributeData(
    const ConcreteDataAttributePath &path, chip::TLV::TLVReader *data,
    const StatusIB &status)
{
    CHIP_ERROR error = status.ToChipError();
    fabric_core_private_attribute_report private_report = {
        .path =
            {
                .endpoint = {.node_id = node->node_id,
                             .endpoint_id = path.mEndpointId},
                .cluster_id = path.mClusterId,
                .attribute_id = path.mAttributeId,
            },
        .data = data,
        .error = error,
    };
    fabric_core_dispatch_private(
        core, [&](const fabric_core_private_listener &listener) {
            if (listener.attribute_reported != nullptr) {
                listener.attribute_reported(core, &private_report,
                                            listener.user_ctx);
            }
        });

    // Per-path failures remain on their attribute reports. Wildcard
    // subscriptions can legitimately include unsupported paths; report_error
    // is reserved for transaction-level failures delivered through OnError.
    if (!fabric_core_has_public_attribute_listener(core)) {
        return;
    }

    attribute_dispatch dispatch = {
        .core = core,
        .report =
            {
                .path = private_report.path,
                .value = {},
                .status = ESP_OK,
            },
    };
    if (error == CHIP_NO_ERROR && data != nullptr) {
        chip::TLV::TLVReader copy;
        copy.Init(*data);
        error = fabric_core_decode_tlv_value_impl(copy, dispatch.report.value);
    }
    dispatch.report.status = fabric_core_from_chip_error(error);
    fabric_core_dispatch_listeners(core, fabric_core_dispatch_attribute, &dispatch);
}

void fabric_core_subscription::OnSubscriptionEstablished(
    chip::SubscriptionId subscription_id)
{
    (void)subscription_id;
    established = true;
    if (report_open && !priming_report_complete) {
        OnReportEnd();
    } else if (priming_report_complete) {
        fabric_core_set_subscription_state(*this, FABRIC_CORE_SUBSCRIPTION_ACTIVE);
    }
}

CHIP_ERROR fabric_core_subscription::OnResubscriptionNeeded(ReadClient *read_client,
                                                        CHIP_ERROR cause)
{
    established = false;
    priming_report_complete = false;
    fabric_core_set_subscription_state(*this, FABRIC_CORE_SUBSCRIPTION_BACKOFF, cause);
    return read_client->DefaultResubscribePolicy(cause);
}

void fabric_core_subscription::OnError(CHIP_ERROR error)
{
    if (report_open) {
        report_open = false;
        report_dispatch dispatch = {
            .core = core,
            .node_id = node->node_id,
            .error = fabric_core_from_chip_error(error),
        };
        fabric_core_dispatch_private(
            core, [&](const fabric_core_private_listener &listener) {
                if (listener.report_end != nullptr) {
                    listener.report_end(core, node->node_id, dispatch.error,
                                        false, listener.user_ctx);
                }
            });
        fabric_core_dispatch_listeners(core, fabric_core_dispatch_report_end,
                                   &dispatch);
    }
    report_error = error;
}

void fabric_core_subscription::OnDone(ReadClient *read_client)
{
    if (client == read_client) {
        chip::Platform::Delete(client);
        client = nullptr;
    }
    established = false;
    priming_report_complete = false;
    fabric_core_set_subscription_state(*this, FABRIC_CORE_SUBSCRIPTION_TERMINATED,
                                   report_error);
}

fabric_core_subscription *fabric_core_get_subscription(fabric_core_node *node)
{
    return static_cast<fabric_core_subscription *>(node->subscription);
}

} // namespace

CHIP_ERROR fabric_core_decode_tlv_value(chip::TLV::TLVReader &reader,
                                    fabric_core_value_t &value)
{
    return fabric_core_decode_tlv_value_impl(reader, value);
}

void fabric_core_private_notify_node_set_changed(fabric_core_handle_t core)
{
    fabric_core_dispatch_private(
        core, [&](const fabric_core_private_listener &listener) {
            if (listener.node_set_changed != nullptr) {
                listener.node_set_changed(core, listener.user_ctx);
            }
        });
}

esp_err_t fabric_core_subscription_start_node(fabric_core_handle_t core,
                                          fabric_core_node *node)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto *subscription = fabric_core_get_subscription(node);
    if (subscription == nullptr) {
        subscription = chip::Platform::New<fabric_core_subscription>(core, node);
        if (subscription == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        node->subscription = subscription;
    }
    fabric_core_set_subscription_state(*subscription,
                                   FABRIC_CORE_SUBSCRIPTION_CONNECTING);
    CHIP_ERROR error = subscription->start();
    if (error != CHIP_NO_ERROR) {
        fabric_core_set_subscription_state(*subscription,
                                       FABRIC_CORE_SUBSCRIPTION_TERMINATED, error);
    }
    return fabric_core_from_chip_error(error);
}

esp_err_t fabric_core_subscriptions_start(fabric_core_handle_t core)
{
    for (size_t index = 0; index < core->node_count; ++index) {
        esp_err_t error =
            fabric_core_subscription_start_node(core, &core->nodes[index]);
        if (error != ESP_OK) {
            fabric_core_subscriptions_stop(core);
            return error;
        }
    }
    return ESP_OK;
}

void fabric_core_subscriptions_stop(fabric_core_handle_t core)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    for (size_t index = 0; index < core->node_count; ++index) {
        auto *subscription = fabric_core_get_subscription(&core->nodes[index]);
        if (subscription != nullptr) {
            subscription->stop();
            chip::Platform::Delete(subscription);
            core->nodes[index].subscription = nullptr;
        }
        core->nodes[index].subscription_state = FABRIC_CORE_SUBSCRIPTION_TERMINATED;
    }
}

esp_err_t fabric_core_subscription_retry(fabric_core_handle_t core, fabric_core_node *node)
{
    (void)core;
    if (node->subscription_state == FABRIC_CORE_SUBSCRIPTION_CONNECTING ||
        node->subscription_state == FABRIC_CORE_SUBSCRIPTION_ACTIVE) {
        return ESP_OK;
    }
    if (node->subscription_state == FABRIC_CORE_SUBSCRIPTION_TERMINATED) {
        return fabric_core_subscription_start_node(core, node);
    }
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto *subscription = fabric_core_get_subscription(node);
    if (subscription == nullptr || subscription->client == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return subscription->client->TriggerResubscribeIfScheduled(
               "Local Fabric manual retry")
               ? ESP_OK
               : ESP_ERR_INVALID_STATE;
}
