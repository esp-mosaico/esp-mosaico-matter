/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "bridge_dev_mngr_private.h"

#include <cstring>

#include "clusters/bridged_device_basic_information/bridged_device_basic_information.h"

using namespace esp_matter;

namespace {

constexpr uint32_t kOnOffLight = UINT32_C(0x0100);
constexpr uint32_t kOutlet = UINT32_C(0x010a);
constexpr uint32_t kDimmableLight = UINT32_C(0x0101);
constexpr uint32_t kColorLight = UINT32_C(0x010d);
constexpr uint32_t kContactSensor = UINT32_C(0x0015);
constexpr uint32_t kOccupancySensor = UINT32_C(0x0107);
constexpr uint32_t kIlluminanceSensor = UINT32_C(0x0106);
constexpr uint32_t kColorControlCluster = UINT32_C(0x0300);

} // namespace

uint32_t bridge::builtin_device_type(const char *profile_id)
{
    if (!profile_id) {
        return 0;
    }
    if (!strcmp(profile_id, BRIDGE_DEV_MNGR_PROFILE_ON_OFF_LIGHT)) {
        return kOnOffLight;
    }
    if (!strcmp(profile_id, BRIDGE_DEV_MNGR_PROFILE_OUTLET)) {
        return kOutlet;
    }
    if (!strcmp(profile_id, BRIDGE_DEV_MNGR_PROFILE_DIMMABLE_LIGHT)) {
        return kDimmableLight;
    }
    if (!strcmp(profile_id, BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT)) {
        return kColorLight;
    }
    if (!strcmp(profile_id, BRIDGE_DEV_MNGR_PROFILE_CONTACT_SENSOR)) {
        return kContactSensor;
    }
    if (!strcmp(profile_id, BRIDGE_DEV_MNGR_PROFILE_OCCUPANCY_SENSOR)) {
        return kOccupancySensor;
    }
    if (!strcmp(profile_id, BRIDGE_DEV_MNGR_PROFILE_ILLUMINANCE_SENSOR)) {
        return kIlluminanceSensor;
    }
    return 0;
}

esp_err_t bridge::build_profile(endpoint_t *endpoint, uint32_t device_type,
                             void *context)
{
    auto *device = static_cast<device_record *>(context);
    if (!endpoint || !device) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error =
        clusters::bridged_device_basic_information::create(endpoint, *device);
    if (error != ESP_OK) {
        return error;
    }
    const uint32_t builtin = builtin_device_type(device->definition.profile_id);
    if (!builtin) {
        if (!device->has_custom_profile ||
            device->profile.device_type != device_type ||
            !device->profile.build_endpoint) {
            return ESP_ERR_INVALID_ARG;
        }
        return device->profile.build_endpoint(endpoint, device->definition.id,
                                              device->profile.user_ctx);
    }
    if (builtin != device_type) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (device_type) {
    case kOnOffLight: {
        endpoint::on_off_light::config_t config;
        config.on_off.on_off = false;
        return endpoint::on_off_light::add(endpoint, &config);
    }
    case kOutlet: {
        endpoint::on_off_plug_in_unit::config_t config;
        config.on_off.on_off = false;
        return endpoint::on_off_plug_in_unit::add(endpoint, &config);
    }
    case kDimmableLight: {
        endpoint::dimmable_light::config_t config;
        config.on_off.on_off = false;
        config.level_control.current_level = 254;
        config.level_control_lighting.start_up_current_level = nullptr;
        return endpoint::dimmable_light::add(endpoint, &config);
    }
    case kColorLight: {
        endpoint::extended_color_light::config_t config;
        config.on_off.on_off = false;
        config.color_control_color_temperature
            .color_temp_physical_min_mireds = 153;
        config.color_control_color_temperature
            .color_temp_physical_max_mireds = 500;
        config.color_control_color_temperature
            .couple_color_temp_to_level_min_mireds = 153;
        config.level_control.current_level = 254;
        config.level_control_lighting.start_up_current_level = nullptr;
        error = endpoint::extended_color_light::add(endpoint, &config);
        if (error != ESP_OK) {
            return error;
        }
        cluster::color_control::feature::hue_saturation::config_t hue_saturation;
        return cluster::color_control::feature::hue_saturation::add(
            cluster::get(endpoint, kColorControlCluster), &hue_saturation);
    }
    case kContactSensor: {
        endpoint::contact_sensor::config_t config;
        return endpoint::contact_sensor::add(endpoint, &config);
    }
    case kOccupancySensor: {
        endpoint::occupancy_sensor::config_t config;
        config.occupancy_sensing.feature_flags =
            cluster::occupancy_sensing::feature::other::get_id();
        return endpoint::occupancy_sensor::add(endpoint, &config);
    }
    case kIlluminanceSensor: {
        endpoint::light_sensor::config_t config;
        config.illuminance_measurement.measured_value = nullptr;
        config.illuminance_measurement.min_measured_value = 1;
        config.illuminance_measurement.max_measured_value = 65534;
        return endpoint::light_sensor::add(endpoint, &config);
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}
