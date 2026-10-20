/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_status_display.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "display_service.h"
#include "esp_check.h"
#include "esp_display_present.h"
#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_display.h"
#include "lvgl.h"
#include "wifi_manager.h"

static const char *TAG = "hub_display";
static esp_display_presenter_t *s_presenter;
static lv_display_t *s_display;
static lv_obj_t *s_status;
static lv_obj_t *s_network;
static lv_obj_t *s_ap;
static lv_obj_t *s_ip;
static lv_obj_t *s_clock;
static lv_obj_t *s_date;
static lv_timer_t *s_timer;
static lv_font_t *s_clock_font;
extern const uint8_t clock_font_start[] asm("_binary_clock_ttf_start");
extern const uint8_t clock_font_end[] asm("_binary_clock_ttf_end");

/* Preserve marquee progress when the periodic snapshot has not changed. */
static void set_label_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

/* Runs on the LVGL task; no additional status worker or copied network state. */
static void update_status(lv_timer_t *timer)
{
    (void)timer;
    wifi_manager_status_t wifi = {0};
    wifi_manager_get_status(&wifi);
    const char *ip = wifi.sta_connected ? wifi.sta_ip : wifi.ap_active ? wifi.ap_ip : "offline";
    size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t external_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t external_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    char network[80];
    snprintf(network, sizeof(network), "Wi-Fi: %s", wifi.sta_connected ? wifi.sta_ssid : "offline");
    set_label_text(s_network, network);
    snprintf(network, sizeof(network), "AP: %s", wifi.ap_active ? wifi.ap_ssid : "off");
    set_label_text(s_ap, network);
    snprintf(network, sizeof(network), "IP: %s", ip ? ip : "offline");
    set_label_text(s_ip, network);
    time_t now = time(NULL);
    struct tm local = {0};
    char clock[16] = "--:--";
    char date[48] = "Waiting for time sync";
    /* Boot uptime is not a calendar date; leave an explicit placeholder until
     * the existing system time service has established a plausible epoch. */
    if (localtime_r(&now, &local) && local.tm_year >= 120) {
        strftime(clock, sizeof(clock), "%H:%M", &local);
        strftime(date, sizeof(date), "%a, %d %b  %Z", &local);
    }
    set_label_text(s_clock, clock);
    set_label_text(s_date, date);
    lv_label_set_text_fmt(s_status,
        "RAM  %u / %u KiB\nPSRAM  %u / %u KiB",
        (unsigned)((internal_total - internal_free) / 1024), (unsigned)(internal_total / 1024),
        (unsigned)((external_total - external_free) / 1024), (unsigned)(external_total / 1024));
}

static bool generation_valid(void *context, uint32_t generation)
{
    return display_service_presenter_validate(context, generation);
}

static void destroy_widgets(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_status = NULL;
    s_network = NULL;
    s_ap = NULL;
    s_ip = NULL;
    s_clock = NULL;
    s_date = NULL;
    if (s_display) {
        (void)esp_lv_adapter_unregister_display(s_display);
        s_display = NULL;
    }
    if (s_clock_font) {
        lv_tiny_ttf_destroy(s_clock_font);
        s_clock_font = NULL;
    }
    (void)esp_lv_adapter_deinit();
}

static esp_err_t quiesce(void *context, uint32_t timeout_ms)
{
    (void)context;
    ESP_RETURN_ON_ERROR(esp_lv_adapter_pause((int32_t)timeout_ms), TAG, "pause status screen");
    esp_err_t error = esp_display_presenter_quiesce(s_presenter, timeout_ms);
    if (error != ESP_OK) {
        (void)esp_lv_adapter_resume();
        return error;
    }
    /* Wait for submitted frames before freeing LVGL's draw buffers. Exclusive
     * Lua/display sessions can then use the adapter, and activation rebuilds this
     * screen from current system state when the baseline is restored. */
    destroy_widgets();
    return ESP_OK;
}

static esp_err_t activate(void *context, esp_display_presenter_t *presenter, uint32_t generation)
{
    ESP_RETURN_ON_FALSE(generation_valid(context, generation), ESP_ERR_INVALID_STATE, TAG, "stale display lease");
    esp_lv_adapter_config_t config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&config), TAG, "initialize LVGL");
    uint16_t rotation = 0;
    (void)display_service_get_rotation(&rotation);
    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_PROFILE(
        display_service_width(), display_service_height(), (esp_lv_adapter_rotation_t)rotation, 32, true);
    s_display = esp_lv_adapter_register_display_with_presenter(&display_config,
        &(esp_lv_adapter_presenter_config_t){
            .presenter = presenter,
            .producer_generation = generation,
            .render_alignment = {.x_pixels = 4, .y_pixels = 4, .width_pixels = 4, .height_pixels = 4},
            .validate_generation = generation_valid,
            .validation_user_ctx = context,
        });
    if (!s_display) {
        destroy_widgets();
        return ESP_ERR_NO_MEM;
    }
    /* LVGL owns the logical dimensions, including any startup rotation. */
    int32_t width = lv_display_get_horizontal_resolution(s_display);
    int32_t height = lv_display_get_vertical_resolution(s_display);
    int32_t short_side = width < height ? width : height;
    int32_t font_size = short_side / 5;
    if (font_size > 96) font_size = 96;
    s_clock_font = lv_tiny_ttf_create_data_ex(clock_font_start,
        (size_t)(clock_font_end - clock_font_start), font_size,
        LV_FONT_KERNING_NONE, 16);
    if (!s_clock_font) {
        destroy_widgets();
        return ESP_ERR_NO_MEM;
    }
    /* The adapter task has not started yet, so widget construction cannot race
     * a render. Its default screen is owned by the registered LVGL display. */
    lv_obj_t *screen = lv_display_get_screen_active(s_display);
    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_t *header = content ? lv_obj_create(content) : NULL;
    lv_obj_t *clock_group = content ? lv_obj_create(content) : NULL;
    if (!content || !header || !clock_group) {
        destroy_widgets();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(content);
    lv_obj_remove_style_all(header);
    lv_obj_remove_style_all(clock_group);
    /* A centered 70% rectangle also stays inside a round panel's perimeter.
     * Flow layout reserves real text heights instead of overlapping anchors. */
    lv_obj_set_size(content, LV_PCT(70), LV_PCT(70));
    lv_obj_center(content);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_BETWEEN,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(clock_group, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(clock_group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clock_group, LV_FLEX_ALIGN_START,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(clock_group, short_side / 60, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(clock_group, LV_OBJ_FLAG_SCROLLABLE);
    s_network = lv_label_create(header);
    s_ap = lv_label_create(header);
    s_ip = lv_label_create(header);
    s_clock = lv_label_create(clock_group);
    s_date = lv_label_create(clock_group);
    s_status = lv_label_create(content);
    s_timer = s_status && s_network && s_ap && s_ip && s_clock && s_date
        ? lv_timer_create(update_status, 1000, NULL) : NULL;
    if (!s_timer) {
        destroy_widgets();
        return ESP_ERR_NO_MEM;
    }
    /* Match Edge Agent's system_ui home hierarchy and dark palette without
     * importing its launcher, task panel or independent display lifecycle. */
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0d0f14), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0x9aa2b0), 0);
    lv_obj_set_style_text_font(screen, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_align(screen, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_network, LV_PCT(100));
    lv_obj_set_width(s_ap, LV_PCT(100));
    lv_obj_set_width(s_ip, LV_PCT(100));
    lv_label_set_long_mode(s_network, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_long_mode(s_ap, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(0xf2f4f8), 0);
    lv_obj_set_style_text_font(s_clock, s_clock_font, 0);
    lv_obj_set_width(s_date, LV_PCT(100));
    lv_label_set_long_mode(s_date, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_status, LV_PCT(100));
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    update_status(s_timer);
    esp_err_t error = esp_lv_adapter_start();
    if (error != ESP_OK) destroy_widgets();
    return error;
}

esp_err_t app_status_display_start(void)
{
    if (s_display) return ESP_OK;
    static const display_service_present_producer_ops_t operations = {
        .quiesce = quiesce, .activate = activate,
    };
    const display_service_present_producer_t producer = {
        .identity = &s_presenter, .ops = &operations, .ctx = &s_presenter,
    };
    esp_lcd_touch_handle_t touch = NULL;
    uint32_t generation = 0;
    ESP_RETURN_ON_ERROR(display_service_presenter_start_baseline(
        &producer, &s_presenter, &touch, &generation), TAG, "start baseline presenter");
    ESP_RETURN_ON_ERROR(activate(&s_presenter, s_presenter, generation), TAG, "start status screen");
    ESP_LOGI(TAG, "Status screen active: %" PRIu32 "x%" PRIu32,
             display_service_width(), display_service_height());
    return ESP_OK;
}
