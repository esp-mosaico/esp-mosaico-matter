/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "sdkconfig.h"

#if CONFIG_APP_CLAW_MOSAIC_GSP_ENABLE
#include "bridge_battery.h"

#include "bq27220.h"
#include "esp_board_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

/* Match Controller's sampling and charge-direction debounce. */
#define SAMPLE_MS 5000U
#define CHARGE_ON_MA 8
#define CHARGE_CONFIRM_SAMPLES 3U
#define MIN_VALID_MV 2500U
#define MAX_VALID_MV 5000U

static const char *TAG = "bridge_battery";
static esp_timer_handle_t s_timer;
static portMUX_TYPE s_sample_lock = portMUX_INITIALIZER_UNLOCKED;
static mosaic_settings_battery_t s_sample;
static bool s_charging;
static bool s_charge_candidate;
static uint8_t s_charge_candidate_samples;
static esp_err_t s_last_read_error;

static esp_err_t read_battery(mosaic_settings_battery_t *sample)
{
    if (!esp_board_manager_check_name("battery_monitor")) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    void *battery = NULL;
    esp_err_t error = esp_board_manager_get_device_handle("battery_monitor", &battery);
    if (error != ESP_OK || battery == NULL) {
        error = esp_board_manager_init_device_by_name("battery_monitor");
        if (error != ESP_OK) return error;
        error = esp_board_manager_get_device_handle("battery_monitor", &battery);
        if (error != ESP_OK) return error;
    }
    if (battery == NULL) return ESP_ERR_INVALID_STATE;

    battery_status_t status = {0};
    error = bq27220_get_battery_status(battery, &status);
    if (error != ESP_OK) return error;
    sample->voltage_mv = bq27220_get_voltage(battery);
    sample->state_of_charge = bq27220_get_state_of_charge(battery);
    if (sample->voltage_mv < MIN_VALID_MV || sample->voltage_mv > MAX_VALID_MV ||
        sample->state_of_charge > 100U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const int16_t current_ma = bq27220_get_current(battery);
    const int16_t average_ma = bq27220_get_avgcurrent(battery);
    /* Cell current is not a USB-present signal. A small negative current
     * under load must not clear charging while the gauge is outside DSG. */
    const bool positive_current = current_ma >= CHARGE_ON_MA || average_ma >= CHARGE_ON_MA;
    const bool candidate = !status.DSG || positive_current;
    if (candidate != s_charge_candidate) {
        s_charge_candidate = candidate;
        s_charge_candidate_samples = 1;
    } else if (s_charge_candidate_samples < CHARGE_CONFIRM_SAMPLES) {
        ++s_charge_candidate_samples;
    }
    if (s_charge_candidate_samples >= CHARGE_CONFIRM_SAMPLES && s_charging != candidate) {
        s_charging = candidate;
        ESP_LOGI(TAG, "charging %s (I=%d mA avg=%d mA DSG=%d FC=%d)",
            s_charging ? "on" : "off", (int)current_ma, (int)average_ma,
            (int)status.DSG, (int)status.FC);
    }

    sample->available = true;
    sample->charging = s_charging;
    sample->current_ma = average_ma;
    sample->time_to_empty_min = s_charging ? UINT16_MAX : bq27220_get_time_to_empty(battery);
    sample->time_to_full_min = s_charging ? bq27220_get_time_to_full(battery) : UINT16_MAX;
    sample->cycle_count = bq27220_get_cycle_count(battery);
    sample->state_of_health = bq27220_get_state_of_health(battery);
    if (sample->state_of_health > 100U) sample->state_of_health = UINT16_MAX;
    return ESP_OK;
}

static bool battery_changed(const mosaic_settings_battery_t *a, const mosaic_settings_battery_t *b)
{
    return a->available != b->available || a->charging != b->charging ||
        a->state_of_charge != b->state_of_charge || a->voltage_mv != b->voltage_mv ||
        a->current_ma != b->current_ma || a->time_to_empty_min != b->time_to_empty_min ||
        a->time_to_full_min != b->time_to_full_min || a->cycle_count != b->cycle_count ||
        a->state_of_health != b->state_of_health;
}

static void sample_and_publish(bool force)
{
    mosaic_settings_battery_t sample = {0};
    esp_err_t error = read_battery(&sample);
    if (error != ESP_OK) {
        /* A failed read breaks the sequence of consecutive charge samples. */
        s_charge_candidate_samples = 0;
        if (error != s_last_read_error) {
            ESP_LOGW(TAG, "sample failed: %s (V=%u mV SoC=%u%%); keeping last valid sample",
                esp_err_to_name(error), (unsigned)sample.voltage_mv, (unsigned)sample.state_of_charge);
        }
        s_last_read_error = error;
        if (force) {
            (void)bridge_battery_get(&sample);
            mosaic_settings_notify_battery(&sample);
        }
        return;
    }
    if (s_last_read_error != ESP_OK) ESP_LOGI(TAG, "battery sampling recovered");
    s_last_read_error = ESP_OK;

    portENTER_CRITICAL(&s_sample_lock);
    const bool publish = force || battery_changed(&s_sample, &sample);
    const bool log_sample = !s_sample.available || s_sample.state_of_charge != sample.state_of_charge ||
        s_sample.charging != sample.charging;
    s_sample = sample;
    portEXIT_CRITICAL(&s_sample_lock);
    if (log_sample) {
        ESP_LOGI(TAG, "SoC=%u%% V=%u mV avg=%d mA charging=%d", (unsigned)sample.state_of_charge,
            (unsigned)sample.voltage_mv, (int)sample.current_ma, (int)sample.charging);
    }
    if (publish) mosaic_settings_notify_battery(&sample);
}

static void battery_timer_cb(void *arg)
{
    (void)arg;
    sample_and_publish(false);
}

esp_err_t bridge_battery_start(void)
{
    if (s_timer != NULL) return ESP_OK;
    const esp_timer_create_args_t args = {
        .callback = battery_timer_cb,
        .name = "bridge_batt",
    };
    esp_timer_handle_t timer = NULL;
    esp_err_t error = esp_timer_create(&args, &timer);
    if (error != ESP_OK) return error;
    sample_and_publish(true);
    error = esp_timer_start_periodic(timer, (uint64_t)SAMPLE_MS * 1000ULL);
    if (error != ESP_OK) {
        (void)esp_timer_delete(timer);
        return error;
    }
    s_timer = timer;
    ESP_LOGI(TAG, "sampler started at %u ms", (unsigned)SAMPLE_MS);
    return ESP_OK;
}

esp_err_t bridge_battery_get(mosaic_settings_battery_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_sample_lock);
    *out = s_sample;
    portEXIT_CRITICAL(&s_sample_lock);
    return ESP_OK;
}
#endif
