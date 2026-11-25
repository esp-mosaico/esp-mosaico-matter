#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "bridge_dev_mngr.h"

typedef enum {
    FRONTEND_DEVICE_LIGHT, FRONTEND_DEVICE_AC, FRONTEND_DEVICE_TV,
    FRONTEND_DEVICE_PLUG, FRONTEND_DEVICE_DOOR, FRONTEND_DEVICE_COUNT
} frontend_backend_device_t;
typedef enum {
    FRONTEND_COMMAND_POWER, FRONTEND_COMMAND_BRIGHTNESS, FRONTEND_COMMAND_KELVIN,
    FRONTEND_COMMAND_AC_TEMPERATURE, FRONTEND_COMMAND_AC_MODE,
    FRONTEND_COMMAND_AC_FAN, FRONTEND_COMMAND_TV_KEY,
    FRONTEND_COMMAND_HUE, FRONTEND_COMMAND_SATURATION
} frontend_backend_command_t;
typedef struct {
    /* DOOR on=true means Matter BooleanState=true (contact closed). */
    bool available, reachable, on_known, on;
    bool supports_power, supports_brightness, supports_kelvin, supports_color;
    /* Hue and saturation use original Matter units, inclusive 0..254. */
    bool hue_known, saturation_known;
    uint8_t hue, saturation;
    bool contact_known, contact_closed, occupancy_known, occupied, illuminance_known;
    double illuminance_lux;
    bool brightness_known;
    uint8_t brightness_percent;
    bool kelvin_known, kelvin_bounds_known;
    uint32_t kelvin, min_kelvin, max_kelvin;
    bool pending;
    /* Per-device completion cannot be hidden by another device completing in
     * the same owner tick. Includes immediate rejection and detach cancel. */
    uint32_t completion_serial;
    esp_err_t completion_error;
    char id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
    char name[BRIDGE_DEV_MNGR_NAME_MAX];
    char profile_id[BRIDGE_DEV_MNGR_PROFILE_ID_MAX];
    uint32_t device_type;
    /* Identity epoch changes even when an identical ID is removed/reinserted. */
    uint32_t generation;
    uint16_t endpoint_id;
} frontend_backend_device_state_t;
typedef frontend_backend_device_state_t frontend_backend_catalog_entry_t;
typedef struct {
    size_t catalog_count;
    bool catalog_ready;
    frontend_backend_catalog_entry_t catalog[BRIDGE_DEV_MNGR_MAX_DEVICES];
    char selected_id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
    /* LIGHT is the explicitly selected endpoint's controls/read-only state.
     * PLUG/DOOR remain compatibility summaries; AC/TV stay unsupported. */
    frontend_backend_device_state_t devices[FRONTEND_DEVICE_COUNT];
    bool ldr_known;
    /* Existing Web display scale: round(lux/4), 0..400 lux, clamped 0..100.
     * Matter does not provide raw LDR voltage or a calibrated LDR percent. */
    uint8_t ldr_percent;
    double illuminance_lux;
    /* Matter endpoint IDs displayed in Automation. Zero means absent. */
    uint16_t pir_endpoint_id, ldr_endpoint_id;
    bool pir_known, pir;
    uint32_t completion_serial;
    esp_err_t completion_error;
    frontend_backend_device_t completion_device;
    char message[96];
} frontend_backend_snapshot_t;

/* Call all three functions on the UI owner task. Callbacks only fill a bounded
 * private mailbox. init may be called again; late old callbacks remain safe.
 * Manager must outlive accepted requests; this adapter does not own it. */
void frontend_backend_init(bridge_dev_mngr_handle_t manager);
void frontend_backend_tick(frontend_backend_snapshot_t *out);
/* ESP_OK means accepted, not completed. Observe completion_serial/error.
 * POWER: 0/1; BRIGHTNESS: 1..100 percent; KELVIN: absolute kelvin within
 * freshly read physical limits. Unsupported AC/TV operations are rejected. */
esp_err_t frontend_backend_command(frontend_backend_device_t device,
                                  frontend_backend_command_t command, int32_t value);

/* Select the detail endpoint by exact ID. Removal never retargets another ID.
 * The LIGHT compatibility channel exposes the selection, including outlets
 * and read-only sensor values. Initial selection is the first real light. */
esp_err_t frontend_backend_select(const char *id);
/* Exact catalog ID write; sensor profiles are read-only. HUE/SATURATION 0..254. */
esp_err_t frontend_backend_command_id(const char *id,
    frontend_backend_command_t command, int32_t value);
