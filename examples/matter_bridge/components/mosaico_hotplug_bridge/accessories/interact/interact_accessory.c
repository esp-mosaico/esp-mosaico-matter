/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "interact_lua.h"
#include "mosaico_hotplug_accessory.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "bsp/subboard.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

static const char *TAG = "mosaico_interact";

/* LED0/1/2 map to three independent on/off lights. MOSAICO_INTERACT_LED_COUNT
 * is 6; indices 3-5 are left unused. */
#define INTERACT_LED_COUNT 3

/* Embedded (NUL-terminated, via EMBED_TXTFILES) from
 * accessories/interact/mosaico_interact_led.lua. */
extern const char kInteractLedDriverSource[] asm("_binary_mosaico_interact_led_lua_start");

static led_strip_handle_t s_handle[MOSAICO_MODULE_MGR_SLOT_COUNT];
static mosaico_module_lease_t s_lease[MOSAICO_MODULE_MGR_SLOT_COUNT];
static char s_device_ids[MOSAICO_MODULE_MGR_SLOT_COUNT][INTERACT_LED_COUNT][BRIDGE_DEV_MNGR_DEVICE_ID_MAX];

static esp_err_t release_leds(mosaico_module_mgr_slot_t slot)
{
    if (s_handle[slot]) {
        esp_err_t err = led_strip_clear(s_handle[slot]);
        if (err != ESP_OK) ESP_LOGW(TAG, "Failed to clear LEDs: %s", esp_err_to_name(err));
        err = led_strip_del(s_handle[slot]);
        if (err != ESP_OK) return err;
        s_handle[slot] = NULL;
    }
    if (s_lease[slot].id) {
        esp_err_t err = mosaico_module_mgr_release(&s_lease[slot]);
        if (err != ESP_OK) return err;
        s_lease[slot] = (mosaico_module_lease_t){0};
    }
    return ESP_OK;
}

static esp_err_t open_leds(mosaico_module_mgr_slot_t slot)
{
    /* Retry cleanup if an earlier detach could not release the hardware. */
    esp_err_t err = release_leds(slot);
    if (err != ESP_OK) return err;

    const mosaico_module_mgr_claim_config_t claim = {
        .expected_type = MOSAICO_BOARD_TYPE_INTERACT,
        .slot = slot,
        .timeout_ms = 1500,
    };
    err = mosaico_module_mgr_claim(&claim, &s_lease[slot]);
    if (err != ESP_OK) return err;

    const led_strip_config_t strip = {
        .strip_gpio_num = bsp_subboard_map_gpio((bsp_subboard_slot_t)slot, GPIO_NUM_15),
        .max_leds = MOSAICO_INTERACT_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
    };
    err = led_strip_new_rmt_device(&strip, &rmt, &s_handle[slot]);
    if (err == ESP_OK) err = led_strip_clear(s_handle[slot]);
    if (err != ESP_OK) {
        esp_err_t release_err = release_leds(slot);
        if (release_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to release LEDs after open failure: %s", esp_err_to_name(release_err));
        }
    }
    return err;
}

static void device_name(uint16_t board_id, uint32_t serial_number, int led_index,
    char *out, size_t out_len)
{
    snprintf(out, out_len, "interact-%04x%08" PRIx32 "-led%d", board_id, serial_number, led_index);
}

static void interact_absent(const mosaico_hotplug_event_t *event);

static bool add_led(mosaico_module_mgr_slot_t slot, const char *name, int led_index,
    char *out_id, size_t out_id_len)
{
    cJSON *args = cJSON_CreateObject();
    if (!args) return false;
    cJSON_AddNumberToObject(args, "slot", slot);
    cJSON_AddNumberToObject(args, "led_index", led_index);
    return mosaico_hotplug_add_virtual_device(name, "on_off_light",
        kInteractLedDriverSource, args, out_id, out_id_len);
}

static esp_err_t interact_present(const mosaico_hotplug_event_t *event)
{
    esp_err_t err = open_leds(event->slot);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open interact module on %s slot: %s",
            mosaico_module_mgr_slot_to_name(event->slot), esp_err_to_name(err));
        return err;
    }

    mosaico_interact_lua_set_handle(event->slot, s_handle[event->slot]);

    cJSON *devices_list = mosaico_hotplug_fetch_devices();
    bool all_ok = true;
    for (int i = 0; i < INTERACT_LED_COUNT; i++) {
        char name[BRIDGE_DEV_MNGR_NAME_MAX];
        device_name(event->board_id, event->serial_number, i, name, sizeof(name));
        char id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX] = {0};
        bool ok = mosaico_hotplug_find_existing_device(devices_list, name, id, sizeof(id));
        if (!ok) ok = add_led(event->slot, name, i, id, sizeof(id));
        if (ok) {
            snprintf(s_device_ids[event->slot][i], sizeof(s_device_ids[event->slot][i]), "%s", id);
            ESP_LOGI(TAG, "Interact module LED%d ready as device %s (%s)", i, id, name);
        } else {
            s_device_ids[event->slot][i][0] = '\0';
            all_ok = false;
            ESP_LOGE(TAG, "Failed to bring up device for interact module LED%d (%s)", i, name);
        }
    }
    cJSON_Delete(devices_list);
    if (!all_ok) {
        ESP_LOGE(TAG, "Rolling back interact module on %s slot after a partial add",
            mosaico_module_mgr_slot_to_name(event->slot));
        interact_absent(event);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void interact_absent(const mosaico_hotplug_event_t *event)
{
    if (!s_handle[event->slot] && !s_lease[event->slot].id) return;

    for (int i = 0; i < INTERACT_LED_COUNT; i++) {
        mosaico_hotplug_remove_device(s_device_ids[event->slot][i]);
        s_device_ids[event->slot][i][0] = '\0';
    }

    mosaico_interact_lua_set_handle(event->slot, NULL);
    esp_err_t close_err = release_leds(event->slot);
    if (close_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to close interact module handle: %s", esp_err_to_name(close_err));
    }
    ESP_LOGI(TAG, "Interact module removed from %s slot", mosaico_module_mgr_slot_to_name(event->slot));
}

const mosaico_hotplug_accessory_t mosaico_hotplug_interact_accessory = {
    .name = "interact",
    .board_type = MOSAICO_BOARD_TYPE_INTERACT,
    .register_lua = mosaico_interact_lua_register,
    .on_present = interact_present,
    .on_absent = interact_absent,
};
