/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "claw_matter_bridge_private.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "soc/soc_caps.h"

namespace claw_matter_bridge
{

esp_err_t bridge_hardware_claim(bridge_entry_t *e)
{
    char keys[BRIDGE_MAX_PINS][32];
    const char *children[BRIDGE_MAX_PINS + 1] = {0};
    char device_key[64];
    for (size_t i = 0; i < e->model.pin_count; i++) {
        const bridge_pin_t *p = &e->model.pins[i];
        if (!GPIO_IS_VALID_GPIO(p->gpio) || (p->mode != BRIDGE_INPUT && !GPIO_IS_VALID_OUTPUT_GPIO(p->gpio)))
            return ESP_ERR_INVALID_ARG;
        children[i] = claw_hw_key_gpio(keys[i], sizeof(keys[i]), p->gpio);
        if (!children[i]) return ESP_ERR_INVALID_ARG;
    }
    snprintf(e->owner, sizeof(e->owner), "matter_bridge/%s", e->model.device.id);
    snprintf(device_key, sizeof(device_key), "matter_bridge:%s", e->model.device.id);
    claw_hw_claim_config_t cfg = {};
    cfg.resource = device_key;
    cfg.owner_tag = e->owner;
    cfg.mode = CLAW_HW_MODE_EXCLUSIVE;
    cfg.sub_resources = children;
    return claw_hw_claim(&cfg, &e->lease);
}

esp_err_t bridge_hardware_close(bridge_entry_t *e)
{
    esp_err_t result = ESP_OK;
    e->access = false;
    e->ready = false;
    for (size_t i = 0; i < e->model.pin_count; i++) {
        const bridge_pin_t *p = &e->model.pins[i];
        if (e->strips[i]) {
            esp_err_t err = led_strip_clear(e->strips[i]);
            if (err == ESP_OK) err = led_strip_del(e->strips[i]);
            if (err != ESP_OK) { result = err; continue; }
            e->strips[i] = NULL;
        }
        if (e->pwm_channels[i] >= 0) {
            int ch = e->pwm_channels[i];
            esp_err_t err = ledc_stop(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(ch), 0);
            if (err != ESP_OK) { result = err; continue; }
            g_bridge->pwm_mask &= ~(1U << ch);
            e->pwm_channels[i] = -1;
        }
        if (e->configured[i]) {
            esp_err_t err;
            const auto gpio = static_cast<gpio_num_t>(p->gpio);
            if (p->mode == BRIDGE_INPUT) err = gpio_reset_pin(gpio);
            else {
                /* Keep the inactive level driven after release; the next
                 * owner can reconfigure it. Active-low relays must not float. */
                err = gpio_set_level(gpio, p->mode == BRIDGE_STRIP ? 0 : p->active_low);
                if (err == ESP_OK) err = gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
            }
            if (err != ESP_OK) result = err;
            else e->configured[i] = false;
        }
    }
    return result;
}

esp_err_t bridge_hardware_open(bridge_entry_t *e)
{
    if (!e->lease) return ESP_ERR_INVALID_STATE;
    e->quiesced = false;
    for (size_t i = 0; i < e->model.pin_count; i++) {
        const bridge_pin_t *p = &e->model.pins[i];
        esp_err_t err = ESP_OK;
        gpio_config_t gc = {};
        gc.pin_bit_mask = UINT64_C(1) << p->gpio;
        gc.mode = p->mode == BRIDGE_INPUT ? GPIO_MODE_INPUT : GPIO_MODE_OUTPUT;
        gc.pull_up_en = p->pull == 1 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        gc.pull_down_en = p->pull == -1 ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
        const auto gpio = static_cast<gpio_num_t>(p->gpio);
        if (p->mode != BRIDGE_INPUT) err = gpio_set_level(gpio, p->active_low);
        if (err == ESP_OK) err = gpio_config(&gc);
        if (err != ESP_OK) goto failed;
        e->configured[i] = true;
        if (p->mode == BRIDGE_PWM) {
            int ch;
            for (ch = 0; ch < SOC_LEDC_CHANNEL_NUM; ch++)
                if (!(g_bridge->pwm_mask & (1U << ch))) break;
            if (ch == SOC_LEDC_CHANNEL_NUM) { err = ESP_ERR_NO_MEM; goto failed; }
            if (!g_bridge->pwm_mask) {
                ledc_timer_config_t timer = {};
                timer.speed_mode = LEDC_LOW_SPEED_MODE;
                timer.duty_resolution = LEDC_TIMER_12_BIT;
                timer.timer_num = LEDC_TIMER_0;
                timer.freq_hz = BRIDGE_PWM_HZ;
                timer.clk_cfg = LEDC_AUTO_CLK;
                err = ledc_timer_config(&timer);
                if (err != ESP_OK) goto failed;
            }
            ledc_channel_config_t cc = {};
            cc.gpio_num = p->gpio;
            cc.speed_mode = LEDC_LOW_SPEED_MODE;
            cc.channel = static_cast<ledc_channel_t>(ch);
            cc.timer_sel = LEDC_TIMER_0;
            cc.duty = 0;
            cc.flags.output_invert = p->active_low;
            err = ledc_channel_config(&cc);
            if (err != ESP_OK) goto failed;
            g_bridge->pwm_mask |= 1U << ch;
            e->pwm_channels[i] = ch;
        } else if (p->mode == BRIDGE_STRIP) {
            led_strip_config_t sc = {};
            sc.strip_gpio_num = p->gpio;
            sc.max_leds = static_cast<uint32_t>(p->led_count);
            sc.led_model = LED_MODEL_WS2812;
            sc.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
            led_strip_rmt_config_t rc = {};
            rc.resolution_hz = 10000000;
            err = led_strip_new_rmt_device(&sc, &rc, &e->strips[i]);
            if (err == ESP_OK) err = led_strip_clear(e->strips[i]);
            if (err != ESP_OK) goto failed;
        }
        continue;
failed:
        bridge_hardware_close(e);
        return err;
    }
    e->access = true;
    e->quiesced = false;
    return ESP_OK;
}

void bridge_hardware_release(bridge_entry_t *e)
{
    if (e->lease && claw_hw_release(e->lease) == ESP_OK) e->lease = NULL;
}

} // namespace claw_matter_bridge
