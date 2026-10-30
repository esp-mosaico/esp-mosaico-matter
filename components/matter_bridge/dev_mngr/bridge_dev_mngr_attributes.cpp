/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "bridge_dev_mngr_private.h"
#include "bridge_control_validation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "clusters/bridged_device_basic_information/bridged_device_basic_information.h"
#include "clusters/occupancy_sensing/occupancy_sensing.h"
#include "clusters/illuminance_measurement/illuminance_measurement.h"
#include "esp_matter_data_model_provider.h"

namespace {

constexpr char kHex[] = "0123456789abcdef";
constexpr auto kLegacyInteger = static_cast<esp_matter_val_type_t>(2);

int hex_digit(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

template <typename T>
bool signed_fits(int64_t value)
{
    return value >= std::numeric_limits<T>::min() &&
           value <= std::numeric_limits<T>::max();
}

template <typename T>
bool unsigned_fits(uint64_t value)
{
    return value <= std::numeric_limits<T>::max();
}

esp_err_t encode_for_attribute(esp_matter::attribute_t *attribute,
                               const bridge_dev_mngr_value_t &source,
                               esp_matter_attr_val_t &destination,
                               uint8_t *octets, size_t octet_capacity)
{
    if (!attribute || source.truncated) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_matter_val_type_t declared =
        esp_matter::attribute::get_val_type(attribute);
    const auto base = static_cast<esp_matter_val_type_t>(
        declared & ~ESP_MATTER_VAL_NULLABLE_BASE);
    if (source.type == BRIDGE_DEV_MNGR_VALUE_NULL) {
        if (!(declared & ESP_MATTER_VAL_NULLABLE_BASE)) {
            return ESP_ERR_INVALID_ARG;
        }
        switch (base) {
        case ESP_MATTER_VAL_TYPE_BOOLEAN:
            destination = esp_matter_attr_val_t(nullable<bool>{});
            break;
        case ESP_MATTER_VAL_TYPE_FLOAT:
            destination = esp_matter_attr_val_t(nullable<float>{});
            break;
        case kLegacyInteger:
        case ESP_MATTER_VAL_TYPE_INT32:
            destination = esp_matter_attr_val_t(nullable<int32_t>{});
            break;
        case ESP_MATTER_VAL_TYPE_INT8:
            destination = esp_matter_attr_val_t(nullable<int8_t>{});
            break;
        case ESP_MATTER_VAL_TYPE_INT16:
            destination = esp_matter_attr_val_t(nullable<int16_t>{});
            break;
        case ESP_MATTER_VAL_TYPE_INT64:
            destination = esp_matter_attr_val_t(nullable<int64_t>{});
            break;
        case ESP_MATTER_VAL_TYPE_UINT8:
        case ESP_MATTER_VAL_TYPE_ENUM8:
        case ESP_MATTER_VAL_TYPE_BITMAP8:
            destination = esp_matter_attr_val_t(nullable<uint8_t>{});
            break;
        case ESP_MATTER_VAL_TYPE_UINT16:
        case ESP_MATTER_VAL_TYPE_ENUM16:
        case ESP_MATTER_VAL_TYPE_BITMAP16:
            destination = esp_matter_attr_val_t(nullable<uint16_t>{});
            break;
        case ESP_MATTER_VAL_TYPE_UINT32:
        case ESP_MATTER_VAL_TYPE_BITMAP32:
            destination = esp_matter_attr_val_t(nullable<uint32_t>{});
            break;
        case ESP_MATTER_VAL_TYPE_UINT64:
            destination = esp_matter_attr_val_t(nullable<uint64_t>{});
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
        }
        destination.type = declared;
        return ESP_OK;
    }
    switch (base) {
    case ESP_MATTER_VAL_TYPE_BOOLEAN:
        if (source.type != BRIDGE_DEV_MNGR_VALUE_BOOL) {
            return ESP_ERR_INVALID_ARG;
        }
        destination = esp_matter_bool(source.data.boolean);
        break;
    case ESP_MATTER_VAL_TYPE_FLOAT:
        if (source.type != BRIDGE_DEV_MNGR_VALUE_FLOAT ||
            !std::isfinite(source.data.float_value)) {
            return ESP_ERR_INVALID_ARG;
        }
        destination = esp_matter_float(
            static_cast<float>(source.data.float_value));
        break;
    case ESP_MATTER_VAL_TYPE_INT8:
        if (source.type != BRIDGE_DEV_MNGR_VALUE_SIGNED ||
            !signed_fits<int8_t>(source.data.signed_value)) {
            return ESP_ERR_INVALID_ARG;
        }
        destination = esp_matter_int8(
            static_cast<int8_t>(source.data.signed_value));
        break;
    case ESP_MATTER_VAL_TYPE_INT16:
        if (source.type != BRIDGE_DEV_MNGR_VALUE_SIGNED ||
            !signed_fits<int16_t>(source.data.signed_value)) {
            return ESP_ERR_INVALID_ARG;
        }
        destination = esp_matter_int16(
            static_cast<int16_t>(source.data.signed_value));
        break;
    case kLegacyInteger:
    case ESP_MATTER_VAL_TYPE_INT32:
        if (source.type != BRIDGE_DEV_MNGR_VALUE_SIGNED ||
            !signed_fits<int32_t>(source.data.signed_value)) {
            return ESP_ERR_INVALID_ARG;
        }
        destination = esp_matter_int32(
            static_cast<int32_t>(source.data.signed_value));
        break;
    case ESP_MATTER_VAL_TYPE_INT64:
        if (source.type != BRIDGE_DEV_MNGR_VALUE_SIGNED) {
            return ESP_ERR_INVALID_ARG;
        }
        destination = esp_matter_int64(source.data.signed_value);
        break;
    case ESP_MATTER_VAL_TYPE_UINT8:
    case ESP_MATTER_VAL_TYPE_ENUM8:
    case ESP_MATTER_VAL_TYPE_BITMAP8:
        if (source.type != BRIDGE_DEV_MNGR_VALUE_UNSIGNED ||
            !unsigned_fits<uint8_t>(source.data.unsigned_value)) {
            return ESP_ERR_INVALID_ARG;
        }
        destination = esp_matter_uint8(
            static_cast<uint8_t>(source.data.unsigned_value));
        destination.type = declared;
        break;
    case ESP_MATTER_VAL_TYPE_UINT16:
    case ESP_MATTER_VAL_TYPE_ENUM16:
    case ESP_MATTER_VAL_TYPE_BITMAP16:
        if (source.type != BRIDGE_DEV_MNGR_VALUE_UNSIGNED ||
            !unsigned_fits<uint16_t>(source.data.unsigned_value)) {
            return ESP_ERR_INVALID_ARG;
        }
        destination = esp_matter_uint16(
            static_cast<uint16_t>(source.data.unsigned_value));
        destination.type = declared;
        break;
    case ESP_MATTER_VAL_TYPE_UINT32:
    case ESP_MATTER_VAL_TYPE_BITMAP32:
        if (source.type != BRIDGE_DEV_MNGR_VALUE_UNSIGNED ||
            !unsigned_fits<uint32_t>(source.data.unsigned_value)) {
            return ESP_ERR_INVALID_ARG;
        }
        destination = esp_matter_uint32(
            static_cast<uint32_t>(source.data.unsigned_value));
        destination.type = declared;
        break;
    case ESP_MATTER_VAL_TYPE_UINT64:
        if (source.type != BRIDGE_DEV_MNGR_VALUE_UNSIGNED) {
            return ESP_ERR_INVALID_ARG;
        }
        destination = esp_matter_uint64(source.data.unsigned_value);
        break;
    case ESP_MATTER_VAL_TYPE_CHAR_STRING:
    case ESP_MATTER_VAL_TYPE_LONG_CHAR_STRING: {
        if (source.type != BRIDGE_DEV_MNGR_VALUE_STRING ||
            !memchr(source.text, 0, sizeof(source.text))) {
            return ESP_ERR_INVALID_ARG;
        }
        const size_t length = strnlen(source.text, sizeof(source.text));
        destination = esp_matter_attr_val_t(
            const_cast<char *>(source.text), static_cast<uint16_t>(length),
            base == ESP_MATTER_VAL_TYPE_LONG_CHAR_STRING);
        break;
    }
    case ESP_MATTER_VAL_TYPE_OCTET_STRING:
    case ESP_MATTER_VAL_TYPE_LONG_OCTET_STRING: {
        if (source.type != BRIDGE_DEV_MNGR_VALUE_OCTETS ||
            !memchr(source.text, 0, sizeof(source.text))) {
            return ESP_ERR_INVALID_ARG;
        }
        const size_t length = strnlen(source.text, sizeof(source.text));
        if ((length & 1U) || length / 2 > octet_capacity) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t index = 0; index < length / 2; ++index) {
            const int high = hex_digit(source.text[index * 2]);
            const int low = hex_digit(source.text[index * 2 + 1]);
            if (high < 0 || low < 0) {
                return ESP_ERR_INVALID_ARG;
            }
            octets[index] = static_cast<uint8_t>((high << 4) | low);
        }
        destination = esp_matter_attr_val_t(
            octets, static_cast<uint16_t>(length / 2),
            base == ESP_MATTER_VAL_TYPE_LONG_OCTET_STRING);
        break;
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    destination.type = declared;
    return ESP_OK;
}

} // namespace

esp_err_t bridge::decode_attribute_value(const esp_matter_attr_val_t &source,
                                      bridge_dev_mngr_value_t &destination)
{
    destination = {};
    if (source.is_nullable() && source.is_null()) {
        destination.type = BRIDGE_DEV_MNGR_VALUE_NULL;
        return ESP_OK;
    }
    switch (source.get_base_type()) {
    case ESP_MATTER_VAL_TYPE_BOOLEAN:
        destination.type = BRIDGE_DEV_MNGR_VALUE_BOOL;
        destination.data.boolean = source.val.b;
        return ESP_OK;
    case ESP_MATTER_VAL_TYPE_FLOAT:
        destination.type = BRIDGE_DEV_MNGR_VALUE_FLOAT;
        destination.data.float_value = source.val.f;
        return ESP_OK;
    case ESP_MATTER_VAL_TYPE_INT8:
        destination.type = BRIDGE_DEV_MNGR_VALUE_SIGNED;
        destination.data.signed_value = source.val.i8;
        return ESP_OK;
    case ESP_MATTER_VAL_TYPE_INT16:
        destination.type = BRIDGE_DEV_MNGR_VALUE_SIGNED;
        destination.data.signed_value = source.val.i16;
        return ESP_OK;
    case kLegacyInteger:
    case ESP_MATTER_VAL_TYPE_INT32:
        destination.type = BRIDGE_DEV_MNGR_VALUE_SIGNED;
        destination.data.signed_value = source.val.i32;
        return ESP_OK;
    case ESP_MATTER_VAL_TYPE_INT64:
        destination.type = BRIDGE_DEV_MNGR_VALUE_SIGNED;
        destination.data.signed_value = source.val.i64;
        return ESP_OK;
    case ESP_MATTER_VAL_TYPE_UINT8:
    case ESP_MATTER_VAL_TYPE_ENUM8:
    case ESP_MATTER_VAL_TYPE_BITMAP8:
        destination.type = BRIDGE_DEV_MNGR_VALUE_UNSIGNED;
        destination.data.unsigned_value = source.val.u8;
        return ESP_OK;
    case ESP_MATTER_VAL_TYPE_UINT16:
    case ESP_MATTER_VAL_TYPE_ENUM16:
    case ESP_MATTER_VAL_TYPE_BITMAP16:
        destination.type = BRIDGE_DEV_MNGR_VALUE_UNSIGNED;
        destination.data.unsigned_value = source.val.u16;
        return ESP_OK;
    case ESP_MATTER_VAL_TYPE_UINT32:
    case ESP_MATTER_VAL_TYPE_BITMAP32:
        destination.type = BRIDGE_DEV_MNGR_VALUE_UNSIGNED;
        destination.data.unsigned_value = source.val.u32;
        return ESP_OK;
    case ESP_MATTER_VAL_TYPE_UINT64:
        destination.type = BRIDGE_DEV_MNGR_VALUE_UNSIGNED;
        destination.data.unsigned_value = source.val.u64;
        return ESP_OK;
    case ESP_MATTER_VAL_TYPE_CHAR_STRING:
    case ESP_MATTER_VAL_TYPE_LONG_CHAR_STRING: {
        destination.type = BRIDGE_DEV_MNGR_VALUE_STRING;
        const size_t copied =
            std::min<size_t>(source.val.a.s, sizeof(destination.text) - 1);
        memcpy(destination.text, source.val.a.b, copied);
        destination.text[copied] = 0;
        destination.truncated = copied != source.val.a.s;
        return ESP_OK;
    }
    case ESP_MATTER_VAL_TYPE_OCTET_STRING:
    case ESP_MATTER_VAL_TYPE_LONG_OCTET_STRING: {
        destination.type = BRIDGE_DEV_MNGR_VALUE_OCTETS;
        const size_t copied =
            std::min<size_t>(source.val.a.s, (sizeof(destination.text) - 1) / 2);
        for (size_t index = 0; index < copied; ++index) {
            destination.text[index * 2] = kHex[source.val.a.b[index] >> 4];
            destination.text[index * 2 + 1] =
                kHex[source.val.a.b[index] & 0x0f];
        }
        destination.text[copied * 2] = 0;
        destination.truncated = copied != source.val.a.s;
        return ESP_OK;
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

const bridge::cluster_adapter *bridge::find_cluster_adapter(uint32_t cluster_id)
{
    static const cluster_adapter *const adapters[] = {
        &clusters::bridged_device_basic_information::adapter,
        &clusters::occupancy_sensing::adapter,
        &clusters::illuminance_measurement::adapter,
    };
    for (const auto *adapter : adapters) {
        if (adapter->cluster_id == cluster_id) {
            return adapter;
        }
    }
    return nullptr;
}

esp_err_t bridge::generic_attribute_read(
    uint16_t endpoint_id, const bridge_dev_mngr_attribute_path_t &path,
    bridge_dev_mngr_value_t &out_value)
{
    esp_matter_attr_val_t value;
    esp_err_t error = esp_matter::attribute::get_val(
        endpoint_id, path.cluster_id, path.attribute_id, &value);
    return error == ESP_OK ? decode_attribute_value(value, out_value) : error;
}

esp_err_t bridge::generic_attribute_report(
    uint16_t endpoint_id, const bridge_dev_mngr_attribute_path_t &path,
    const bridge_dev_mngr_value_t &value)
{
    auto *attribute = esp_matter::attribute::get(
        endpoint_id, path.cluster_id, path.attribute_id);
    if (!attribute) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t octets[(BRIDGE_DEV_MNGR_VALUE_TEXT_MAX - 1) / 2]{};
    esp_matter_attr_val_t encoded;
    esp_err_t error =
        encode_for_attribute(attribute, value, encoded, octets, sizeof(octets));
    if (error != ESP_OK) {
        return error;
    }
    error = esp_matter::attribute::set_val(endpoint_id, path.cluster_id,
                                           path.attribute_id, &encoded, true);
    if (error == ESP_ERR_NOT_FINISHED) {
        error = ESP_OK;
    }
    if (error == ESP_OK) {
        esp_matter::data_model::provider::get_instance().ReportAttributeChanged(
            chip::app::AttributePathParams(endpoint_id, path.cluster_id,
                                           path.attribute_id));
    }
    return error;
}

esp_err_t bridge::attribute_read(device_record &record,
                              const bridge_dev_mngr_attribute_path_t &path,
                              bridge_dev_mngr_value_t &out_value)
{
    if (record.has_custom_profile && record.profile.read_attribute) {
        esp_err_t error = record.profile.read_attribute(
            record.endpoint_id, &path, &out_value, record.profile.user_ctx);
        if (error != ESP_ERR_NOT_SUPPORTED) {
            return error;
        }
    }
    const auto *adapter = find_cluster_adapter(path.cluster_id);
    return adapter && adapter->read
               ? adapter->read(record.endpoint_id, path, out_value)
               : generic_attribute_read(record.endpoint_id, path, out_value);
}

esp_err_t bridge::attribute_report(device_record &record,
                                const bridge_dev_mngr_attribute_path_t &path,
                                const bridge_dev_mngr_value_t &value)
{
    if (record.has_custom_profile && record.profile.report_attribute) {
        esp_err_t error = record.profile.report_attribute(
            record.endpoint_id, &path, &value, record.profile.user_ctx);
        if (error != ESP_ERR_NOT_SUPPORTED) {
            return error;
        }
    }
    const auto *adapter = find_cluster_adapter(path.cluster_id);
    return adapter && adapter->report
               ? adapter->report(record.endpoint_id, path, value)
               : generic_attribute_report(record.endpoint_id, path, value);
}

esp_err_t bridge::attribute_write(device_record &record,
                                 const bridge_dev_mngr_attribute_path_t &path,
                                 const bridge_dev_mngr_value_t &value)
{
    esp_err_t error = validate_control(record.definition.profile_id, path, value);
    if (error != ESP_OK) return error;
    if (path.cluster_id == 0x0300 && path.attribute_id == 7) {
        bridge_dev_mngr_value_t minimum{}, maximum{};
        error = generic_attribute_read(record.endpoint_id, {0x0300, 0x400b}, minimum);
        if (error != ESP_OK) return error;
        error = generic_attribute_read(record.endpoint_id, {0x0300, 0x400c}, maximum);
        if (error != ESP_OK) return error;
        if (minimum.type != BRIDGE_DEV_MNGR_VALUE_UNSIGNED ||
            maximum.type != BRIDGE_DEV_MNGR_VALUE_UNSIGNED ||
            value.data.unsigned_value < minimum.data.unsigned_value ||
            value.data.unsigned_value > maximum.data.unsigned_value) return ESP_ERR_INVALID_ARG;
    }
    auto *attribute = esp_matter::attribute::get(record.endpoint_id,
                                                 path.cluster_id, path.attribute_id);
    if (!attribute) return ESP_ERR_NOT_FOUND;
    uint8_t octets[(BRIDGE_DEV_MNGR_VALUE_TEXT_MAX - 1) / 2]{};
    esp_matter_attr_val_t encoded;
    error = encode_for_attribute(attribute, value, encoded, octets, sizeof(octets));
    if (error != ESP_OK) return error;
    error = esp_matter::attribute::update(record.endpoint_id, path.cluster_id,
                                         path.attribute_id, &encoded);
    if (error != ESP_OK || path.cluster_id != 0x0300) return error;

    // The same listener used by remote commands selects the physical driver's
    // HS or temperature implementation from ColorMode.
    auto mode = esp_matter_enum8(path.attribute_id == 7 ? 2 : 0);
    error = esp_matter::attribute::update(record.endpoint_id, 0x0300, 8, &mode);
    if (error != ESP_OK) return error;
    return esp_matter::attribute::update(record.endpoint_id, 0x0300, 0x4001, &mode);
}
