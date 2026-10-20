/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_manager_private.h"

#include "esp_check.h"

#include <cstdlib>
#include <cstring>

namespace
{

constexpr uint32_t kStoreMagic = UINT32_C(0x4c464435); // LFD5
constexpr uint16_t kStoreVersion = 5;
const char *const kTag = "fabric_manager_store";

struct store_header
{
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;
    fabric_manager_device_id_t next_device_id;
};

struct store_record
{
    fabric_manager_device_id_t device_id;
    uint64_t node_id;
    uint16_t root_endpoint_id;
    char type[FABRIC_MANAGER_DEVICE_TYPE_MAX];
    char name[FABRIC_MANAGER_NAME_MAX];
    char room[FABRIC_MANAGER_ROOM_MAX];
};

bool validate_records(const store_record *records, size_t count,
                      fabric_manager_device_id_t next_device_id)
{
    if (count > CONFIG_FABRIC_MANAGER_MAX_DEVICES || next_device_id == 0)
        return false;
    for (size_t index = 0; index < count; ++index) {
        const auto &record = records[index];
        if (record.device_id == 0 || record.device_id >= next_device_id ||
            record.node_id == 0 || record.root_endpoint_id == 0 ||
            strnlen(record.type, sizeof(record.type)) == sizeof(record.type) ||
            strnlen(record.name, sizeof(record.name)) == sizeof(record.name) ||
            strnlen(record.room, sizeof(record.room)) == sizeof(record.room)) {
            return false;
        }
        for (size_t other = index + 1; other < count; ++other) {
            if (records[other].device_id == record.device_id ||
                (records[other].node_id == record.node_id &&
                 records[other].root_endpoint_id == record.root_endpoint_id)) {
                return false;
            }
        }
    }
    return true;
}

void copy_to_device(const store_record &record, data_model::device &device)
{
    device.id = record.device_id;
    device.node_id = record.node_id;
    device.root_endpoint_id = record.root_endpoint_id;
    memcpy(device.type, record.type, sizeof(device.type));
    memcpy(device.name, record.name, sizeof(device.name));
    memcpy(device.room, record.room, sizeof(device.room));
}

void copy_to_record(const data_model::device &device, store_record &record)
{
    record.device_id = device.id;
    record.node_id = device.node_id;
    record.root_endpoint_id = device.root_endpoint_id;
    memcpy(record.type, device.type, sizeof(record.type));
    memcpy(record.name, device.name, sizeof(record.name));
    memcpy(record.room, device.room, sizeof(record.room));
}

} // namespace

esp_err_t data_model::store::load(fabric_manager_handle_t manager, data_model::device **out_devices,
                            size_t *out_device_count)
{
    ESP_RETURN_ON_FALSE(manager && out_devices && out_device_count,
                        ESP_ERR_INVALID_ARG, kTag, "invalid load output");
    *out_devices = nullptr;
    *out_device_count = 0;

    size_t size = 0;
    esp_err_t error = nvs_get_blob(manager->nvs, FABRIC_MANAGER_NVS_KEY, nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        manager->next_device_id = 1;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(error, kTag, "query state");
    ESP_RETURN_ON_FALSE(size >= sizeof(store_header), ESP_ERR_INVALID_SIZE,
                        kTag, "state is truncated");

    auto *bytes = static_cast<uint8_t *>(malloc(size));
    ESP_RETURN_ON_FALSE(bytes, ESP_ERR_NO_MEM, kTag, "allocate state");
    error = nvs_get_blob(manager->nvs, FABRIC_MANAGER_NVS_KEY, bytes, &size);
    if (error != ESP_OK) {
        free(bytes);
        return error;
    }

    const auto *header = reinterpret_cast<const store_header *>(bytes);
    const size_t expected_size =
        sizeof(*header) +
        static_cast<size_t>(header->record_count) * sizeof(store_record);
    const auto *records =
        reinterpret_cast<const store_record *>(bytes + sizeof(*header));
    if (header->magic != kStoreMagic || header->version != kStoreVersion ||
        size != expected_size ||
        !validate_records(records, header->record_count,
                          header->next_device_id)) {
        free(bytes);
        return ESP_ERR_INVALID_VERSION;
    }

    auto *devices = header->record_count
                        ? static_cast<data_model::device *>(calloc(
                              header->record_count, sizeof(data_model::device)))
                        : nullptr;
    if (header->record_count && !devices) {
        free(bytes);
        return ESP_ERR_NO_MEM;
    }
    for (size_t index = 0; index < header->record_count; ++index) {
        copy_to_device(records[index], devices[index]);
    }
    manager->next_device_id = header->next_device_id;
    *out_devices = devices;
    *out_device_count = header->record_count;
    free(bytes);
    return ESP_OK;
}

esp_err_t data_model::store::save(fabric_manager_handle_t manager, const data_model::device *devices,
                            size_t device_count,
                            fabric_manager_device_id_t next_device_id)
{
    ESP_RETURN_ON_FALSE(manager && (devices || device_count == 0) &&
                            device_count <= CONFIG_FABRIC_MANAGER_MAX_DEVICES,
                        ESP_ERR_INVALID_ARG, kTag, "invalid state");
    const size_t size =
        sizeof(store_header) + device_count * sizeof(store_record);
    auto *bytes = static_cast<uint8_t *>(calloc(1, size));
    ESP_RETURN_ON_FALSE(bytes, ESP_ERR_NO_MEM, kTag, "allocate state");
    auto *header = reinterpret_cast<store_header *>(bytes);
    auto *records = reinterpret_cast<store_record *>(bytes + sizeof(*header));
    header->magic = kStoreMagic;
    header->version = kStoreVersion;
    header->record_count = static_cast<uint16_t>(device_count);
    header->next_device_id = next_device_id;
    for (size_t index = 0; index < device_count; ++index) {
        copy_to_record(devices[index], records[index]);
    }
    if (!validate_records(records, device_count, next_device_id)) {
        free(bytes);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = nvs_set_blob(manager->nvs, FABRIC_MANAGER_NVS_KEY, bytes, size);
    free(bytes);
    return error == ESP_OK ? nvs_commit(manager->nvs) : error;
}
