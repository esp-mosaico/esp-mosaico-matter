/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "claw_matter_bridge_model.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace claw_matter_bridge
{

static const char *const s_types[] = {
    "on_off_light", "outlet", "dimmable_light", "color_light", "contact_sensor", "occupancy_sensor"
};
const char *bridge_type_name(bridge_device_type_t type)
{
    return type >= BRIDGE_DEVICE_LIGHT && type <= BRIDGE_DEVICE_OCCUPANCY_SENSOR
        ? s_types[type] : "unknown";
}
bool bridge_gpio_allowed(int pin, uint64_t mask)
{
    return pin >= 0 && pin < 64 && ((mask >> pin) & 1);
}
static esp_err_t invalid(char *error, size_t size, const char *message)
{
    if (error && size) snprintf(error, size, "%s", message);
    return ESP_ERR_INVALID_ARG;
}
static bool integer(const cJSON *j, int min, int max)
{
    return cJSON_IsNumber(j) && isfinite(j->valuedouble) &&
        j->valuedouble >= min && j->valuedouble <= max && floor(j->valuedouble) == j->valuedouble;
}
static const bridge_pin_t *role(const bridge_model_t *m, const char *name, bridge_pin_mode_t mode)
{
    for (size_t i = 0; i < m->pin_count; i++)
        if (!strcmp(m->pins[i].role, name) && m->pins[i].mode == mode) return &m->pins[i];
    return NULL;
}
esp_err_t bridge_model_parse(const cJSON *json, uint64_t mask, bridge_model_t *out,
    char *error, size_t error_size)
{
    if (!out || !cJSON_IsObject(json)) return invalid(error, error_size, "Expected device object");
    memset(out, 0, sizeof(*out));
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    const cJSON *pins = cJSON_GetObjectItemCaseSensitive(json, "pins");
    if (!cJSON_IsString(name) || !name->valuestring[0] || strlen(name->valuestring) >= sizeof(out->device.name))
        return invalid(error, error_size, "name must contain 1..32 bytes");
    strcpy(out->device.name, name->valuestring);
    bool matched = false;
    for (int i = 0; cJSON_IsString(type) && i < (int)(sizeof(s_types) / sizeof(s_types[0])); i++) {
        if (!strcmp(type->valuestring, s_types[i])) {
            out->type = static_cast<bridge_device_type_t>(i);
            matched = true;
            break;
        }
    }
    if (!matched) return invalid(error, error_size, "Unsupported Matter type");
    static const char *const profiles[] = {
        BRIDGE_DEV_MNGR_PROFILE_ON_OFF_LIGHT,
        BRIDGE_DEV_MNGR_PROFILE_OUTLET,
        BRIDGE_DEV_MNGR_PROFILE_DIMMABLE_LIGHT,
        BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT,
        BRIDGE_DEV_MNGR_PROFILE_CONTACT_SENSOR,
        BRIDGE_DEV_MNGR_PROFILE_OCCUPANCY_SENSOR,
    };
    static const uint32_t device_types[] = {
        UINT32_C(0x0100), UINT32_C(0x010a), UINT32_C(0x0101),
        UINT32_C(0x010d), UINT32_C(0x0015), UINT32_C(0x0107),
    };
    snprintf(out->device.profile_id, sizeof(out->device.profile_id), "%s",
             profiles[out->type]);
    out->device.device_type = device_types[out->type];
    int count = cJSON_GetArraySize(pins);
    if (!cJSON_IsArray(pins) || count < 1 || count > BRIDGE_MAX_PINS) {
        return invalid(error, error_size, "pins must contain 1..8 declarations");
    }
    for (int i = 0; i < count; i++) {
        const cJSON *p = cJSON_GetArrayItem(pins, i);
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(p, "role");
        const cJSON *g = cJSON_GetObjectItemCaseSensitive(p, "gpio");
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(p, "mode");
        if (!cJSON_IsObject(p) || !cJSON_IsString(r) || !r->valuestring[0] ||
            strlen(r->valuestring) >= sizeof(out->pins[i].role) || !integer(g, 0, 63) ||
            !bridge_gpio_allowed(g->valueint, mask) || !cJSON_IsString(mode))
            return invalid(error, error_size, "Invalid role, mode or GPIO outside the allowlist");
        bridge_pin_t *pin = &out->pins[i];
        strcpy(pin->role, r->valuestring);
        pin->gpio = g->valueint;
        if (!strcmp(mode->valuestring, "input")) pin->mode = BRIDGE_INPUT;
        else if (!strcmp(mode->valuestring, "output")) pin->mode = BRIDGE_OUTPUT;
        else if (!strcmp(mode->valuestring, "pwm")) pin->mode = BRIDGE_PWM;
        else if (!strcmp(mode->valuestring, "strip")) pin->mode = BRIDGE_STRIP;
        else return invalid(error, error_size, "mode must be input, output, pwm or strip");
        const cJSON *low = cJSON_GetObjectItemCaseSensitive(p, "active_low");
        if (low && !cJSON_IsBool(low)) return invalid(error, error_size, "active_low must be boolean");
        pin->active_low = cJSON_IsTrue(low);
        const cJSON *pull = cJSON_GetObjectItemCaseSensitive(p, "pull");
        if (pull) {
            if (pin->mode != BRIDGE_INPUT || !cJSON_IsString(pull))
                return invalid(error, error_size, "pull is only valid on inputs");
            if (!strcmp(pull->valuestring, "up")) pin->pull = 1;
            else if (!strcmp(pull->valuestring, "down")) pin->pull = -1;
            else if (strcmp(pull->valuestring, "none")) return invalid(error, error_size, "Invalid input pull");
        }
        if (pin->mode == BRIDGE_STRIP) {
            const cJSON *leds = cJSON_GetObjectItemCaseSensitive(p, "led_count");
            if (!integer(leds, 1, BRIDGE_MAX_LEDS) || pin->active_low)
                return invalid(error, error_size, "strip needs led_count 1..256 and active_low=false");
            pin->led_count = leds->valueint;
        }
        for (int j = 0; j < i; j++)
            if (out->pins[j].gpio == pin->gpio || !strcmp(out->pins[j].role, pin->role))
                return invalid(error, error_size, "Duplicate GPIO or role");
        out->pin_count++;
    }
    bool ok = false;
    switch (out->type) {
    case BRIDGE_DEVICE_LIGHT:
    case BRIDGE_DEVICE_OUTLET: ok = role(out, "power", BRIDGE_OUTPUT) != NULL; break;
    case BRIDGE_DEVICE_DIMMABLE_LIGHT: ok = role(out, "level", BRIDGE_PWM) != NULL; break;
    case BRIDGE_DEVICE_COLOR_LIGHT:
        ok = role(out, "color", BRIDGE_STRIP) || (role(out, "red", BRIDGE_PWM) &&
            role(out, "green", BRIDGE_PWM) && role(out, "blue", BRIDGE_PWM)); break;
    default: ok = role(out, "sensor", BRIDGE_INPUT) != NULL; break;
    }
    if (!ok) return invalid(error, error_size, "Required hardware roles missing for this Matter type");
    if (error && error_size) error[0] = 0;
    return ESP_OK;
}

esp_err_t bridge_atomic_write(const char *path, const void *data, size_t size)
{
    char temp[256];
    if (!path || !data || snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp))
        return ESP_ERR_INVALID_ARG;
    /* Picolibc lazily allocates an internal-RAM mutex on first stdio use and
     * aborts if that allocation fails. File descriptors let storage failures
     * propagate normally even while LLM/Matter activity exhausts that heap. */
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return ESP_FAIL;
    size_t written = 0;
    bool ok = true;
    while (written < size) {
        ssize_t n = write(fd, (const char *)data + written, size - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        written += (size_t)n;
    }
    if (fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(temp, path) == 0) return ESP_OK;
    unlink(temp);
    return ESP_FAIL;
}
char *bridge_read_file(const char *path, size_t limit)
{
    if (!path) return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    char *data = NULL;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
        (size_t)st.st_size <= limit && (size_t)st.st_size < SIZE_MAX) {
        size_t size = (size_t)st.st_size;
        data = static_cast<char *>(malloc(size + 1));
        if (data) {
            size_t received = 0;
            while (received < size) {
                ssize_t n = read(fd, data + received, size - received);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { free(data); data = NULL; break; }
                received += (size_t)n;
            }
            if (data) data[size] = 0;
        }
    }
    if (close(fd) != 0) { free(data); data = NULL; }
    return data;
}

} // namespace claw_matter_bridge
