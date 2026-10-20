/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "fabric_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MATTER_VM_TEXT_MAX 64U
#define MATTER_VM_VALUE_MAX 96U
#define MATTER_VM_ACTION_ARGUMENT_MAX 2U
#define MATTER_VM_ENUM_MAX 12U

typedef struct {
    fabric_manager_device_id_t id;
    char type[MATTER_VM_TEXT_MAX];
    char name[FABRIC_MANAGER_NAME_MAX];
    char room[FABRIC_MANAGER_ROOM_MAX];
    bool online;
} matter_vm_device_t;

typedef enum {
    MATTER_VM_VALUE_NULL = 0,
    MATTER_VM_VALUE_BOOLEAN,
    MATTER_VM_VALUE_NUMBER,
    MATTER_VM_VALUE_STRING,
    MATTER_VM_VALUE_OTHER,
} matter_vm_value_kind_t;

typedef struct {
    uint16_t endpoint;
    char endpoint_name[MATTER_VM_TEXT_MAX];
    char cluster[FABRIC_MANAGER_CLUSTER_KEY_MAX];
    char key[FABRIC_MANAGER_MEMBER_KEY_MAX];
    char name[MATTER_VM_TEXT_MAX];
    char display_value[MATTER_VM_VALUE_MAX];
    char unit[MATTER_VM_TEXT_MAX];
    matter_vm_value_kind_t value_kind;
    bool boolean_value;
    double number_value;
    char string_value[MATTER_VM_VALUE_MAX];
} matter_vm_property_t;

typedef enum {
    MATTER_VM_ARGUMENT_BOOLEAN = 0,
    MATTER_VM_ARGUMENT_INTEGER,
    MATTER_VM_ARGUMENT_NUMBER,
    MATTER_VM_ARGUMENT_STRING,
} matter_vm_argument_kind_t;

typedef struct {
    char key[FABRIC_MANAGER_MEMBER_KEY_MAX];
    matter_vm_argument_kind_t kind;
    bool has_min;
    bool has_max;
    double min;
    double max;
    size_t enum_count;
    char enum_values[MATTER_VM_ENUM_MAX][MATTER_VM_TEXT_MAX];
    int32_t enum_ids[MATTER_VM_ENUM_MAX];
} matter_vm_argument_t;

typedef struct {
    uint16_t endpoint;
    char cluster[FABRIC_MANAGER_CLUSTER_KEY_MAX];
    char key[FABRIC_MANAGER_MEMBER_KEY_MAX];
    size_t argument_count;
    matter_vm_argument_t arguments[MATTER_VM_ACTION_ARGUMENT_MAX];
} matter_vm_action_t;

typedef struct {
    matter_vm_device_t device;
    matter_vm_property_t *properties;
    size_t property_count;
    matter_vm_action_t *actions;
    size_t action_count;
} matter_vm_detail_t;

esp_err_t matter_vm_parse_devices(const char *json, matter_vm_device_t *devices,
                                  size_t capacity, size_t *out_count);
esp_err_t matter_vm_parse_detail(const char *json,
                                 matter_vm_detail_t *out_detail);
void matter_vm_free_detail(matter_vm_detail_t *detail);

const matter_vm_property_t *matter_vm_find_property(
    const matter_vm_detail_t *detail, uint16_t endpoint, const char *cluster,
    const char *key);
const matter_vm_action_t *matter_vm_find_action(
    const matter_vm_detail_t *detail, const char *cluster, const char *key);

#ifdef __cplusplus
}
#endif
