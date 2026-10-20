/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_core_private.h"

#include "esp_matter.h"
#include "esp_matter_client.h"
#include "esp_matter_controller_client.h"
#include "json_to_tlv.h"

#include <app/ReadClient.h>
#include <app/WriteClient.h>
#include <controller/CommissioneeDeviceProxy.h>
#include <lib/core/TLV.h>
#include <platform/CHIPDeviceLayer.h>

#include <cstdlib>
#include <cstring>

namespace {

using chip::Messaging::ExchangeManager;
using chip::SessionHandle;
using chip::app::AttributePathParams;
using chip::app::ConcreteDataAttributePath;
using chip::app::ReadClient;
using chip::app::StatusIB;
using chip::app::WriteClient;
using esp_matter::client::interaction::custom_encodable_type;

class operation_context {
public:
    operation_context(fabric_core_handle_t core, fabric_core_request_id_t request_id,
                      fabric_core_operation_t operation, uint64_t node_id,
                      fabric_core_request_completion_cb_t completion_cb,
                      void *user_ctx)
        : core(core), request_id(request_id), operation(operation),
          node_id(node_id), completion_cb(completion_cb), user_ctx(user_ctx),
          connected_callback(on_connected, this),
          connection_failure_callback(on_connection_failure, this)
    {
    }

    virtual ~operation_context() = default;

    esp_err_t start_connection()
    {
        esp_err_t error = fabric_core_request_begin(core);
        if (error != ESP_OK) {
            delete this;
            return error;
        }
        pending = true;
        auto *commissioner = esp_matter::controller::matter_controller_client::
            get_instance().get_commissioner();
        CHIP_ERROR connection_error = commissioner->GetConnectedDevice(
            node_id, &connected_callback, &connection_failure_callback);
        if (connection_error != CHIP_NO_ERROR) {
            abandon();
            return fabric_core_from_chip_error(connection_error);
        }
        return ESP_OK;
    }

    esp_err_t mark_pending()
    {
        esp_err_t error = fabric_core_request_begin(core);
        pending = error == ESP_OK;
        return error;
    }

    void abandon()
    {
        if (pending) {
            pending = false;
            fabric_core_request_end(core);
        }
        delete this;
    }

    void finish(esp_err_t error, const fabric_core_value_t *value = nullptr)
    {
        if (finished) {
            return;
        }
        finished = true;
        fabric_core_request_result_t result = {
            .request_id = request_id,
            .operation = operation,
            .error = error,
            .node_id = node_id,
            .value = {},
        };
        if (value != nullptr) {
            result.value = *value;
        }

        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        completion_cb(core, &result, user_ctx);
        pending = false;
        fabric_core_request_end(core);
        delete this;
    }

protected:
    virtual void connected(ExchangeManager &exchange_manager,
                           const SessionHandle &session) = 0;

    fabric_core_handle_t core;
    fabric_core_request_id_t request_id;
    fabric_core_operation_t operation;
    uint64_t node_id;

private:
    static void on_connected(void *context, ExchangeManager &exchange_manager,
                             const SessionHandle &session)
    {
        static_cast<operation_context *>(context)->connected(exchange_manager,
                                                             session);
    }

    static void on_connection_failure(void *context,
                                      const chip::ScopedNodeId &peer_id,
                                      CHIP_ERROR error)
    {
        (void)peer_id;
        static_cast<operation_context *>(context)->finish(
            fabric_core_from_chip_error(error));
    }

    fabric_core_request_completion_cb_t completion_cb;
    void *user_ctx;
    chip::Callback::Callback<chip::OnDeviceConnected> connected_callback;
    chip::Callback::Callback<chip::OnDeviceConnectionFailure>
        connection_failure_callback;
    bool pending = false;
    bool finished = false;
};

class read_operation final : public operation_context,
                             public ReadClient::Callback {
public:
    read_operation(fabric_core_handle_t core, const fabric_core_attribute_path_t &path,
                   fabric_core_request_id_t request_id,
                   fabric_core_request_completion_cb_t completion_cb,
                   void *user_ctx)
        : operation_context(core, request_id,
                            FABRIC_CORE_OPERATION_READ_ATTRIBUTE,
                            path.endpoint.node_id, completion_cb, user_ctx),
          path(path.endpoint.endpoint_id, path.cluster_id, path.attribute_id)
    {
    }

private:
    void connected(ExchangeManager &exchange_manager,
                   const SessionHandle &session) override
    {
        chip::OperationalDeviceProxy device(&exchange_manager, session);
        esp_err_t error = esp_matter::client::interaction::read::send_request(
            &device, &path, 1, nullptr, 0, *this);
        if (error != ESP_OK) {
            finish(error);
        }
    }

    void OnAttributeData(const ConcreteDataAttributePath &attribute_path,
                         chip::TLV::TLVReader *data,
                         const StatusIB &status) override
    {
        (void)attribute_path;
        CHIP_ERROR error = status.ToChipError();
        if (error == CHIP_NO_ERROR && data != nullptr) {
            chip::TLV::TLVReader copy;
            copy.Init(*data);
            error = fabric_core_decode_tlv_value(copy, value);
        } else if (error == CHIP_NO_ERROR) {
            error = CHIP_ERROR_INVALID_TLV_ELEMENT;
        }
        result_error = fabric_core_from_chip_error(error);
    }

    void OnError(CHIP_ERROR error) override
    {
        result_error = fabric_core_from_chip_error(error);
    }

    void OnDone(ReadClient *client) override
    {
        (void)client;
        finish(result_error, result_error == ESP_OK ? &value : nullptr);
    }

    AttributePathParams path;
    fabric_core_value_t value = {};
    esp_err_t result_error = ESP_FAIL;
};

class value_encodable final : public chip::app::DataModel::EncodableToTLV {
public:
    explicit value_encodable(const fabric_core_value_t &value) : value(value) {}

    CHIP_ERROR EncodeTo(chip::TLV::TLVWriter &writer,
                        chip::TLV::Tag tag) const override
    {
        if (value.type == FABRIC_CORE_VALUE_JSON) {
            return esp_matter::json_to_tlv(value.text, writer, tag) == ESP_OK
                ? CHIP_NO_ERROR
                : CHIP_ERROR_INVALID_ARGUMENT;
        }

        chip::TLV::TLVType container_type;
        ReturnErrorOnFailure(writer.StartContainer(
            tag, chip::TLV::kTLVType_Structure, container_type));
        ReturnErrorOnFailure(
            encode_scalar(writer, chip::TLV::ContextTag(0)));
        return writer.EndContainer(container_type);
    }

private:
    CHIP_ERROR encode_octets(chip::TLV::TLVWriter &writer,
                             chip::TLV::Tag tag) const
    {
        const size_t text_length = strnlen(value.text, sizeof(value.text));
        VerifyOrReturnError((text_length & 1U) == 0,
                            CHIP_ERROR_INVALID_ARGUMENT);
        uint8_t bytes[FABRIC_CORE_TEXT_MAX / 2] = {};
        for (size_t index = 0; index < text_length / 2; ++index) {
            const int high = hex_digit(value.text[index * 2]);
            const int low = hex_digit(value.text[index * 2 + 1]);
            VerifyOrReturnError(high >= 0 && low >= 0,
                                CHIP_ERROR_INVALID_ARGUMENT);
            bytes[index] = static_cast<uint8_t>((high << 4) | low);
        }
        return writer.PutBytes(tag, bytes,
                               static_cast<uint32_t>(text_length / 2));
    }

    CHIP_ERROR encode_scalar(chip::TLV::TLVWriter &writer,
                             chip::TLV::Tag tag) const
    {
        switch (value.type) {
        case FABRIC_CORE_VALUE_NULL:
            return writer.PutNull(tag);
        case FABRIC_CORE_VALUE_BOOL:
            return writer.Put(tag, value.data.boolean);
        case FABRIC_CORE_VALUE_SIGNED:
            return writer.Put(tag, value.data.signed_value);
        case FABRIC_CORE_VALUE_UNSIGNED:
            return writer.Put(tag, value.data.unsigned_value);
        case FABRIC_CORE_VALUE_FLOAT:
            return writer.Put(tag, value.data.float_value);
        case FABRIC_CORE_VALUE_STRING:
            return writer.PutString(tag, value.text);
        case FABRIC_CORE_VALUE_OCTETS:
            return encode_octets(writer, tag);
        case FABRIC_CORE_VALUE_JSON:
            return CHIP_ERROR_INCORRECT_STATE;
        default:
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
    }

    static int hex_digit(char digit)
    {
        if (digit >= '0' && digit <= '9') {
            return digit - '0';
        }
        if (digit >= 'a' && digit <= 'f') {
            return digit - 'a' + 10;
        }
        if (digit >= 'A' && digit <= 'F') {
            return digit - 'A' + 10;
        }
        return -1;
    }

    fabric_core_value_t value;
};

class write_operation final : public operation_context,
                              public WriteClient::Callback {
public:
    write_operation(fabric_core_handle_t core, const fabric_core_attribute_path_t &path,
                    const fabric_core_value_t &value,
                    fabric_core_request_id_t request_id,
                    fabric_core_request_completion_cb_t completion_cb,
                    void *user_ctx)
        : operation_context(core, request_id,
                            FABRIC_CORE_OPERATION_WRITE_ATTRIBUTE,
                            path.endpoint.node_id, completion_cb, user_ctx),
          path(path.endpoint.endpoint_id, path.cluster_id, path.attribute_id),
          value(value)
    {
    }

private:
    void connected(ExchangeManager &exchange_manager,
                   const SessionHandle &session) override
    {
        chip::OperationalDeviceProxy device(&exchange_manager, session);
        esp_err_t error = esp_matter::client::interaction::write::send_request(
            &device, path, value, *this, chip::NullOptional);
        if (error != ESP_OK) {
            finish(error);
        }
    }

    void OnResponse(const WriteClient *client,
                    const ConcreteDataAttributePath &attribute_path,
                    StatusIB status) override
    {
        (void)client;
        (void)attribute_path;
        result_error = fabric_core_from_chip_error(status.ToChipError());
    }

    void OnError(const WriteClient *client, CHIP_ERROR error) override
    {
        (void)client;
        result_error = fabric_core_from_chip_error(error);
    }

    void OnDone(WriteClient *client) override
    {
        (void)client;
        finish(result_error);
    }

    AttributePathParams path;
    value_encodable value;
    esp_err_t result_error = ESP_FAIL;
};

class invoke_operation final : public operation_context {
public:
    invoke_operation(fabric_core_handle_t core, const fabric_core_command_path_t &path,
                     const char *arguments_json,
                     fabric_core_request_id_t request_id,
                     fabric_core_request_completion_cb_t completion_cb,
                     void *user_ctx)
        : operation_context(core, request_id,
                            FABRIC_CORE_OPERATION_INVOKE_COMMAND,
                            path.endpoint.node_id, completion_cb, user_ctx),
          endpoint_id(path.endpoint.endpoint_id), cluster_id(path.cluster_id),
          command_id(path.command_id), arguments_json(strdup(arguments_json))
    {
    }

    invoke_operation(const invoke_operation &) = delete;
    invoke_operation &operator=(const invoke_operation &) = delete;

    ~invoke_operation() override { free(arguments_json); }
    bool valid() const { return arguments_json != nullptr; }

private:
    void connected(ExchangeManager &exchange_manager,
                   const SessionHandle &session) override
    {
        chip::OperationalDeviceProxy device(&exchange_manager, session);
        chip::app::CommandPathParams command_path = {
            endpoint_id, 0, cluster_id, command_id,
            chip::app::CommandPathFlags::kEndpointIdValid,
        };
        esp_err_t error = esp_matter::client::interaction::invoke::send_request(
            this, &device, command_path, arguments_json,
            [](void *context, const chip::app::ConcreteCommandPath &path,
               const StatusIB &status, chip::TLV::TLVReader *response) {
                (void)path;
                auto *self = static_cast<invoke_operation *>(context);
                fabric_core_value_t value = {};
                esp_err_t error = fabric_core_from_chip_error(status.ToChipError());
                if (error == ESP_OK && response != nullptr) {
                    chip::TLV::TLVReader copy;
                    copy.Init(*response);
                    error = fabric_core_from_chip_error(
                        fabric_core_decode_tlv_value(copy, value));
                }
                self->finish(error, response != nullptr && error == ESP_OK
                                        ? &value
                                        : nullptr);
            },
            [](void *context, CHIP_ERROR error) {
                static_cast<invoke_operation *>(context)->finish(
                    fabric_core_from_chip_error(error));
            },
            chip::NullOptional);
        if (error != ESP_OK) {
            finish(error);
        }
    }

    uint16_t endpoint_id;
    uint32_t cluster_id;
    uint32_t command_id;
    char *arguments_json;
};

class remove_operation final : public operation_context {
public:
    remove_operation(fabric_core_handle_t core, uint64_t node_id, bool force,
                     fabric_core_request_id_t request_id,
                     fabric_core_request_completion_cb_t completion_cb,
                     void *user_ctx)
        : operation_context(core, request_id, FABRIC_CORE_OPERATION_REMOVE_NODE,
                            node_id, completion_cb, user_ctx), force(force)
    {
    }

    void removed(CHIP_ERROR chip_error)
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        esp_err_t error = fabric_core_from_chip_error(chip_error);
        if (error == ESP_OK || force) {
            fabric_core_subscriptions_stop(core);
            size_t index = 0;
            while (index < core->node_count &&
                   core->nodes[index].node_id != node_id) {
                ++index;
            }
            if (index == core->node_count) {
                error = ESP_ERR_NOT_FOUND;
            } else {
                for (size_t move = index + 1; move < core->node_count; ++move) {
                    core->nodes[move - 1] = core->nodes[move];
                }
                memset(&core->nodes[core->node_count - 1], 0,
                       sizeof(core->nodes[0]));
                --core->node_count;
            }
            if (error == ESP_OK || force) {
                esp_err_t store_error = fabric_core_store_nodes(core);
                if (store_error != ESP_OK) {
                    error = store_error;
                } else {
                    fabric_core_private_notify_node_set_changed(core);
                    if (force) {
                        error = ESP_OK;
                    }
                }
            }
            bool restart = core->lifecycle == FABRIC_CORE_RUNNING;
            if (restart) {
                esp_err_t restart_error = fabric_core_subscriptions_start(core);
                if (error == ESP_OK) {
                    error = restart_error;
                }
            }
        }
        finish(error);
    }

private:
    void connected(ExchangeManager &, const SessionHandle &) override {}
    bool force;
};

remove_operation *s_remove_operation;

void fabric_core_remove_complete(chip::NodeId node_id, CHIP_ERROR error)
{
    (void)node_id;
    remove_operation *operation = s_remove_operation;
    s_remove_operation = nullptr;
    if (operation != nullptr) {
        operation->removed(error);
    }
}

void fabric_core_force_remove_work(intptr_t context)
{
    auto *operation = reinterpret_cast<remove_operation *>(context);
    s_remove_operation = nullptr;
    operation->removed(CHIP_ERROR_INTERNAL);
}

template <typename Operation, typename... Arguments>
esp_err_t fabric_core_start_operation(Arguments &&...arguments)
{
    auto *operation = new (std::nothrow) Operation(
        static_cast<Arguments &&>(arguments)...);
    if (operation == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    return operation->start_connection();
}

} // namespace

esp_err_t fabric_core_chip_read_attribute(
    fabric_core_handle_t core, const fabric_core_attribute_path_t *path,
    fabric_core_request_id_t request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (fabric_core_find_node(core, path->endpoint.node_id) == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    return fabric_core_start_operation<read_operation>(
        core, *path, request_id, completion_cb, user_ctx);
}

esp_err_t fabric_core_chip_write_attribute(
    fabric_core_handle_t core, const fabric_core_attribute_path_t *path,
    const fabric_core_value_t *value, fabric_core_request_id_t request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (fabric_core_find_node(core, path->endpoint.node_id) == nullptr ||
        value->type > FABRIC_CORE_VALUE_JSON) {
        return value->type > FABRIC_CORE_VALUE_JSON ? ESP_ERR_INVALID_ARG
                                                : ESP_ERR_NOT_FOUND;
    }
    return fabric_core_start_operation<write_operation>(
        core, *path, *value, request_id, completion_cb, user_ctx);
}

esp_err_t fabric_core_chip_invoke_command(
    fabric_core_handle_t core, const fabric_core_command_path_t *path,
    const char *arguments_json, fabric_core_request_id_t request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (fabric_core_find_node(core, path->endpoint.node_id) == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    auto *operation = new (std::nothrow) invoke_operation(
        core, *path, arguments_json, request_id, completion_cb, user_ctx);
    if (operation == nullptr || !operation->valid()) {
        delete operation;
        return ESP_ERR_NO_MEM;
    }
    return operation->start_connection();
}

esp_err_t fabric_core_chip_remove_node(
    fabric_core_handle_t core, uint64_t node_id, bool force,
    fabric_core_request_id_t request_id,
    fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (fabric_core_find_node(core, node_id) == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_remove_operation != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    auto *operation = new (std::nothrow) remove_operation(
        core, node_id, force, request_id, completion_cb, user_ctx);
    if (operation == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t pending_error = operation->mark_pending();
    if (pending_error != ESP_OK) {
        delete operation;
        return pending_error;
    }
    s_remove_operation = operation;
    esp_err_t error = esp_matter::controller::matter_controller_client::
        get_instance().unpair(node_id, fabric_core_remove_complete);
    if (error == ESP_OK) {
        return ESP_OK;
    }
    if (!force) {
        s_remove_operation = nullptr;
        operation->abandon();
        return error;
    }
    CHIP_ERROR schedule_error = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        fabric_core_force_remove_work, reinterpret_cast<intptr_t>(operation));
    if (schedule_error != CHIP_NO_ERROR) {
        s_remove_operation = nullptr;
        operation->abandon();
        return fabric_core_from_chip_error(schedule_error);
    }
    return ESP_OK;
}
