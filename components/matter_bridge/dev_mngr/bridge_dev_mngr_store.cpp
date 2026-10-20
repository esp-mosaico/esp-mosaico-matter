/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "bridge_dev_mngr_private.h"

#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kStoreMagic = UINT32_C(0x4d423031); // MB01
constexpr uint16_t kStoreVersion = 1;

struct store_header {
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;
};

struct store_record {
    bridge_dev_mngr_device_definition_t definition;
    uint16_t endpoint_id;
    uint8_t state;
    uint8_t reserved;
};

bool terminated(const char *text, size_t size)
{
    return text && memchr(text, 0, size);
}

bool valid_definition(const bridge_dev_mngr_device_definition_t &definition)
{
    return definition.id[0] && definition.name[0] && definition.profile_id[0] &&
           definition.device_type &&
           terminated(definition.id, sizeof(definition.id)) &&
           terminated(definition.name, sizeof(definition.name)) &&
           terminated(definition.profile_id, sizeof(definition.profile_id));
}

bool validate(const store_record *records, size_t count)
{
    if (count > BRIDGE_DEV_MNGR_MAX_DEVICES) {
        return false;
    }
    for (size_t index = 0; index < count; ++index) {
        const auto &record = records[index];
        if (!valid_definition(record.definition) ||
            record.state >
                static_cast<uint8_t>(bridge::catalog_state::removing)) {
            return false;
        }
        for (size_t other = index + 1; other < count; ++other) {
            if (!strcmp(record.definition.id, records[other].definition.id) ||
                (record.endpoint_id &&
                 record.endpoint_id == records[other].endpoint_id)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

esp_err_t bridge::store::load(bridge_dev_mngr_handle_t dev_mngr)
{
    size_t size = 0;
    esp_err_t error = nvs_get_blob(dev_mngr->nvs, BRIDGE_DEV_MNGR_NVS_KEY, nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    if (size < sizeof(store_header)) {
        return ESP_ERR_INVALID_SIZE;
    }
    auto *bytes = static_cast<uint8_t *>(malloc(size));
    if (!bytes) {
        return ESP_ERR_NO_MEM;
    }
    error = nvs_get_blob(dev_mngr->nvs, BRIDGE_DEV_MNGR_NVS_KEY, bytes, &size);
    if (error != ESP_OK) {
        free(bytes);
        return error;
    }
    const auto *header = reinterpret_cast<const store_header *>(bytes);
    const size_t expected =
        sizeof(*header) + header->record_count * sizeof(store_record);
    const auto *records = reinterpret_cast<const store_record *>(
        bytes + sizeof(*header));
    if (header->magic != kStoreMagic || header->version != kStoreVersion ||
        expected != size || !validate(records, header->record_count)) {
        free(bytes);
        return ESP_ERR_INVALID_VERSION;
    }
    for (size_t index = 0; index < header->record_count; ++index) {
        auto &device = dev_mngr->devices[index];
        device.definition = records[index].definition;
        device.endpoint_id = records[index].endpoint_id;
        device.persistent_state =
            static_cast<bridge::catalog_state>(records[index].state);
    }
    free(bytes);
    return ESP_OK;
}

esp_err_t bridge::store::save(bridge_dev_mngr_handle_t dev_mngr)
{
    size_t count = 0;
    for (const auto &device : dev_mngr->devices) {
        count += device.definition.id[0] ? 1U : 0U;
    }
    const size_t size = sizeof(store_header) + count * sizeof(store_record);
    auto *bytes = static_cast<uint8_t *>(calloc(1, size));
    if (!bytes) {
        return ESP_ERR_NO_MEM;
    }
    auto *header = reinterpret_cast<store_header *>(bytes);
    auto *records = reinterpret_cast<store_record *>(bytes + sizeof(*header));
    header->magic = kStoreMagic;
    header->version = kStoreVersion;
    header->record_count = static_cast<uint16_t>(count);
    size_t output = 0;
    for (const auto &device : dev_mngr->devices) {
        if (!device.definition.id[0]) {
            continue;
        }
        records[output].definition = device.definition;
        records[output].endpoint_id = device.endpoint_id;
        records[output].state = static_cast<uint8_t>(device.persistent_state);
        ++output;
    }
    if (!validate(records, count)) {
        free(bytes);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error =
        nvs_set_blob(dev_mngr->nvs, BRIDGE_DEV_MNGR_NVS_KEY, bytes, size);
    free(bytes);
    return error == ESP_OK ? nvs_commit(dev_mngr->nvs) : error;
}

esp_err_t bridge::store::erase(bridge_dev_mngr_handle_t dev_mngr)
{
    esp_err_t error = nvs_erase_all(dev_mngr->nvs);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;
    }
    return error == ESP_OK ? nvs_commit(dev_mngr->nvs) : error;
}
