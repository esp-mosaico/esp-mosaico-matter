/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "bridge_core.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_DEV_MNGR_MAX_DEVICES CONFIG_MATTER_BRIDGE_MAX_DEVICES
#define BRIDGE_DEV_MNGR_DEVICE_ID_MAX 33U
#define BRIDGE_DEV_MNGR_NAME_MAX 33U
#define BRIDGE_DEV_MNGR_PROFILE_ID_MAX 40U
#define BRIDGE_DEV_MNGR_VALUE_TEXT_MAX 256U

#define BRIDGE_DEV_MNGR_PROFILE_ON_OFF_LIGHT "on_off_light"
#define BRIDGE_DEV_MNGR_PROFILE_OUTLET "outlet"
#define BRIDGE_DEV_MNGR_PROFILE_DIMMABLE_LIGHT "dimmable_light"
#define BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT "color_light"
#define BRIDGE_DEV_MNGR_PROFILE_CONTACT_SENSOR "contact_sensor"
#define BRIDGE_DEV_MNGR_PROFILE_OCCUPANCY_SENSOR "occupancy_sensor"
#define BRIDGE_DEV_MNGR_PROFILE_ILLUMINANCE_SENSOR "illuminance_sensor"

typedef struct bridge_dev_mngr *bridge_dev_mngr_handle_t;
typedef uint32_t bridge_dev_mngr_request_id_t;
typedef uint32_t bridge_dev_mngr_listener_id_t;

/** Stable persistent definition of one local bridged device. */
typedef struct {
    char id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
    char name[BRIDGE_DEV_MNGR_NAME_MAX];
    char profile_id[BRIDGE_DEV_MNGR_PROFILE_ID_MAX];
    uint32_t device_type;
} bridge_dev_mngr_device_definition_t;

/** Snapshot of persistent identity and current runtime presence. */
typedef struct {
    bridge_dev_mngr_device_definition_t definition;
    uint16_t endpoint_id;
    bool active;
    bool reachable;
} bridge_dev_mngr_device_info_t;

typedef struct {
    uint32_t cluster_id;
    uint32_t attribute_id;
} bridge_dev_mngr_attribute_path_t;

typedef enum {
    BRIDGE_DEV_MNGR_VALUE_NULL = 0,
    BRIDGE_DEV_MNGR_VALUE_BOOL,
    BRIDGE_DEV_MNGR_VALUE_SIGNED,
    BRIDGE_DEV_MNGR_VALUE_UNSIGNED,
    BRIDGE_DEV_MNGR_VALUE_FLOAT,
    BRIDGE_DEV_MNGR_VALUE_STRING,
    BRIDGE_DEV_MNGR_VALUE_OCTETS,
    BRIDGE_DEV_MNGR_VALUE_JSON,
} bridge_dev_mngr_value_type_t;

/** Bounded value used at the public Matter attribute boundary. */
typedef struct {
    bridge_dev_mngr_value_type_t type;
    bool truncated;
    union {
        bool boolean;
        int64_t signed_value;
        uint64_t unsigned_value;
        double float_value;
    } data;
    char text[BRIDGE_DEV_MNGR_VALUE_TEXT_MAX];
} bridge_dev_mngr_value_t;

typedef esp_err_t (*bridge_dev_mngr_endpoint_builder_fn)(
    void *endpoint, const char *device_id, void *user_ctx);
typedef esp_err_t (*bridge_dev_mngr_profile_read_fn)(
    uint16_t endpoint_id, const bridge_dev_mngr_attribute_path_t *path,
    bridge_dev_mngr_value_t *out_value, void *user_ctx);
typedef esp_err_t (*bridge_dev_mngr_profile_report_fn)(
    uint16_t endpoint_id, const bridge_dev_mngr_attribute_path_t *path,
    const bridge_dev_mngr_value_t *value, void *user_ctx);

/** Runtime-only implementation for a custom persistent profile id. */
typedef struct {
    char id[BRIDGE_DEV_MNGR_PROFILE_ID_MAX];
    uint32_t device_type;
    bridge_dev_mngr_endpoint_builder_fn build_endpoint;
    bridge_dev_mngr_profile_read_fn read_attribute;
    bridge_dev_mngr_profile_report_fn report_attribute;
    void *user_ctx;
} bridge_dev_mngr_profile_t;

typedef enum {
    BRIDGE_DEV_MNGR_UPDATE_NAME = UINT32_C(1) << 0,
    BRIDGE_DEV_MNGR_UPDATE_REACHABLE = UINT32_C(1) << 1,
} bridge_dev_mngr_device_update_field_t;

typedef struct {
    uint32_t fields;
    char name[BRIDGE_DEV_MNGR_NAME_MAX];
    bool reachable;
} bridge_dev_mngr_device_update_t;

typedef enum {
    BRIDGE_DEV_MNGR_OPERATION_REGISTER = 0,
    BRIDGE_DEV_MNGR_OPERATION_UPDATE,
    BRIDGE_DEV_MNGR_OPERATION_ACTIVATE,
    BRIDGE_DEV_MNGR_OPERATION_DEACTIVATE,
    BRIDGE_DEV_MNGR_OPERATION_REMOVE,
    BRIDGE_DEV_MNGR_OPERATION_READ_ATTRIBUTE,
    BRIDGE_DEV_MNGR_OPERATION_REPORT_ATTRIBUTE,
    BRIDGE_DEV_MNGR_OPERATION_WRITE_ATTRIBUTE,
} bridge_dev_mngr_operation_t;

typedef struct {
    bridge_dev_mngr_request_id_t request_id;
    bridge_dev_mngr_operation_t operation;
    esp_err_t error;
    bridge_dev_mngr_device_info_t device;
    bridge_dev_mngr_attribute_path_t path;
    bridge_dev_mngr_value_t value;
} bridge_dev_mngr_request_result_t;

/**
 * Called once on the Matter task for each accepted request.
 *
 * Completion may run before the submitting API returns. The result is valid
 * only during this callback; user_ctx must remain valid until it returns.
 */
typedef void (*bridge_dev_mngr_request_completion_cb_t)(
    bridge_dev_mngr_handle_t dev_mngr, const bridge_dev_mngr_request_result_t *result,
    void *user_ctx);

/** Remote Matter or local control attribute change delivered after it commits. */
typedef struct {
    char device_id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
    uint16_t endpoint_id;
    bridge_dev_mngr_attribute_path_t path;
    bridge_dev_mngr_value_t value;
} bridge_dev_mngr_attribute_change_t;

typedef struct {
    void (*device_set_changed)(bridge_dev_mngr_handle_t dev_mngr, void *user_ctx);
    void (*attribute_changed)(bridge_dev_mngr_handle_t dev_mngr,
                              const bridge_dev_mngr_attribute_change_t *change,
                              void *user_ctx);
    void *user_ctx;
} bridge_dev_mngr_listener_t;

/** Creates and attaches the persistent device manager to a stopped core. */
esp_err_t bridge_dev_mngr_create(bridge_core_handle_t core,
                                 bridge_dev_mngr_handle_t *out_dev_mngr);

/** Detaches and deletes a device manager before its core starts. */
esp_err_t bridge_dev_mngr_delete(bridge_dev_mngr_handle_t dev_mngr);

esp_err_t bridge_dev_mngr_add_listener(bridge_dev_mngr_handle_t dev_mngr,
                                       const bridge_dev_mngr_listener_t *listener,
                                       bridge_dev_mngr_listener_id_t *out_listener_id);
esp_err_t bridge_dev_mngr_remove_listener(bridge_dev_mngr_handle_t dev_mngr,
                                          bridge_dev_mngr_listener_id_t listener_id);

/** Returns one catalog snapshot by stable id. */
esp_err_t bridge_dev_mngr_get_device(bridge_dev_mngr_handle_t dev_mngr,
                                     const char *device_id,
                                     bridge_dev_mngr_device_info_t *out_device);

/** Lists catalog snapshots using count-then-fill semantics. */
esp_err_t bridge_dev_mngr_get_devices(bridge_dev_mngr_handle_t dev_mngr,
                                      bridge_dev_mngr_device_info_t *devices,
                                      size_t capacity, size_t *out_count);

esp_err_t bridge_dev_mngr_register_device(
    bridge_dev_mngr_handle_t dev_mngr, const bridge_dev_mngr_device_definition_t *definition,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx);

esp_err_t bridge_dev_mngr_update_device(
    bridge_dev_mngr_handle_t dev_mngr, const char *device_id,
    const bridge_dev_mngr_device_update_t *update,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx);

/** custom_profile must be NULL for built-in profiles. */
esp_err_t bridge_dev_mngr_activate_device(
    bridge_dev_mngr_handle_t dev_mngr, const char *device_id,
    const bridge_dev_mngr_profile_t *custom_profile,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx);

esp_err_t bridge_dev_mngr_deactivate_device(
    bridge_dev_mngr_handle_t dev_mngr, const char *device_id,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx);

esp_err_t bridge_dev_mngr_remove_device(
    bridge_dev_mngr_handle_t dev_mngr, const char *device_id,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx);

esp_err_t bridge_dev_mngr_read_attribute(
    bridge_dev_mngr_handle_t dev_mngr, const char *device_id,
    const bridge_dev_mngr_attribute_path_t *path,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx);

esp_err_t bridge_dev_mngr_report_attribute(
    bridge_dev_mngr_handle_t dev_mngr, const char *device_id,
    const bridge_dev_mngr_attribute_path_t *path, const bridge_dev_mngr_value_t *value,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx);

/**
 * Controls an active, reachable built-in light or outlet on the Matter task.
 * Values use Matter attribute units. Supports OnOff, CurrentLevel (1..254),
 * CurrentHue/CurrentSaturation (0..254) and ColorTemperatureMireds (within the
 * endpoint's physical limits). Color writes also select the matching color
 * mode. Successful changes notify drivers and Matter subscriptions.
 * Sensor reports must continue to use report_attribute(), which does not
 * notify control listeners. Completion follows the other asynchronous APIs.
 */
esp_err_t bridge_dev_mngr_write_attribute(
    bridge_dev_mngr_handle_t dev_mngr, const char *device_id,
    const bridge_dev_mngr_attribute_path_t *path, const bridge_dev_mngr_value_t *value,
    bridge_dev_mngr_request_id_t *out_request_id,
    bridge_dev_mngr_request_completion_cb_t completion_cb, void *user_ctx);

#ifdef __cplusplus
}
#endif
