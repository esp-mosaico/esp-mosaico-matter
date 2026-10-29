/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "interact_inputs.h"
#include "interact_light.h"

#include "bsp/subboard.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"

typedef struct {
    bool active, pir_configured, filter_valid, sample_failed;
    gpio_num_t pir_gpio;
    adc_oneshot_unit_handle_t adc;
    adc_channel_t channel;
    int unit_index;
    double filtered_mv;
} interact_inputs_t;

static interact_inputs_t s_inputs[MOSAICO_MODULE_MGR_SLOT_COUNT];
static adc_oneshot_unit_handle_t s_adc[SOC_ADC_PERIPH_NUM];
static unsigned s_adc_refs[SOC_ADC_PERIPH_NUM];
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;

// Called by the native adapter before opening the input resources.
void mosaico_interact_inputs_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
}

static bool valid_slot(mosaico_module_mgr_slot_t slot)
{
    return slot >= MOSAICO_MODULE_MGR_SLOT_LEFT && slot < MOSAICO_MODULE_MGR_SLOT_COUNT && s_lock;
}

esp_err_t mosaico_interact_inputs_open(mosaico_module_mgr_slot_t slot)
{
    if (!valid_slot(slot)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    interact_inputs_t *inputs = &s_inputs[slot];
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (inputs->pir_configured || inputs->adc) goto done;

    inputs->pir_gpio = bsp_subboard_map_gpio((bsp_subboard_slot_t)slot, GPIO_NUM_4);
    const gpio_config_t pir = {
        .pin_bit_mask = BIT64(inputs->pir_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_reset_pin(inputs->pir_gpio);
    if (err != ESP_OK) goto done;
    // Track partial initialization so the caller can roll it back.
    inputs->pir_configured = true;
    err = gpio_config(&pir);
    if (err != ESP_OK) goto done;

    adc_unit_t unit;
    const gpio_num_t light_gpio = bsp_subboard_map_gpio((bsp_subboard_slot_t)slot, GPIO_NUM_53);
    err = adc_oneshot_io_to_channel(light_gpio, &unit, &inputs->channel);
    if (err != ESP_OK) goto done;
    inputs->unit_index = unit == ADC_UNIT_1 ? 0 : 1;
    if (!s_adc[inputs->unit_index]) {
        const adc_oneshot_unit_init_cfg_t cfg = {
            .unit_id = unit,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        err = adc_oneshot_new_unit(&cfg, &s_adc[inputs->unit_index]);
        if (err != ESP_OK) goto done;
    }
    inputs->adc = s_adc[inputs->unit_index];
    s_adc_refs[inputs->unit_index]++;
    const adc_oneshot_chan_cfg_t channel = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
#if SOC_ADC_ATTEN_NUM <= 1
        .atten = ADC_ATTEN_DB_0,
#else
        .atten = ADC_ATTEN_DB_12,
#endif
    };
    err = adc_oneshot_config_channel(inputs->adc, inputs->channel, &channel);
    if (err == ESP_OK) {
        inputs->active = true;
        inputs->filter_valid = false;
        inputs->sample_failed = false;
    }
done:
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t mosaico_interact_inputs_close(mosaico_module_mgr_slot_t slot)
{
    if (!valid_slot(slot)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    interact_inputs_t *inputs = &s_inputs[slot];
    inputs->active = false;
    esp_err_t err = ESP_OK;
    if (inputs->pir_configured) {
        err = gpio_reset_pin(inputs->pir_gpio);
        if (err != ESP_OK) goto done;
        inputs->pir_configured = false;
    }
    if (inputs->adc) {
        const int index = inputs->unit_index;
        if (s_adc_refs[index] == 1) {
            err = adc_oneshot_del_unit(s_adc[index]);
            if (err != ESP_OK) goto done;
            s_adc[index] = NULL;
        }
        s_adc_refs[index]--;
        inputs->adc = NULL;
    }
    inputs->filter_valid = false;
done:
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t mosaico_interact_pir_read(mosaico_module_mgr_slot_t slot, bool *occupied)
{
    if (!valid_slot(slot) || !occupied) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const interact_inputs_t *inputs = &s_inputs[slot];
    esp_err_t err = inputs->active ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) *occupied = gpio_get_level(inputs->pir_gpio) != 0;
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t mosaico_interact_light_read(mosaico_module_mgr_slot_t slot, double *millivolts)
{
    if (!valid_slot(slot) || !millivolts) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    interact_inputs_t *inputs = &s_inputs[slot];
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!inputs->active) goto done;
    int raw = 0;
    double sample_mv = 0;
    err = adc_oneshot_read(inputs->adc, inputs->channel, &raw);
    if (err == ESP_OK) {
#if CONFIG_IDF_TARGET_ESP32S31
        if (!mosaico_interact_s31_light_voltage(raw, inputs->channel, &sample_mv)) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
#else
        // Other targets need their own calibrated ADC voltage conversion.
        err = ESP_ERR_NOT_SUPPORTED;
#endif
    }
    if (err != ESP_OK) {
        inputs->filter_valid = false;
        if (!inputs->sample_failed) {
            ESP_LOGW("mosaico_interact", "LDR sample failed or out of range: %s (raw=%d)", esp_err_to_name(err), raw);
        }
        inputs->sample_failed = true;
        goto done;
    }
    inputs->sample_failed = false;
    // Publish the upper bound immediately on saturation; otherwise retain
    // the existing 1/4-new-sample filter without rounding to a percent.
    inputs->filtered_mv = inputs->filter_valid && sample_mv < 2000.0
        ? (inputs->filtered_mv * 3 + sample_mv) / 4 : sample_mv;
    inputs->filter_valid = true;
    *millivolts = inputs->filtered_mv;
done:
    xSemaphoreGive(s_lock);
    return err;
}
