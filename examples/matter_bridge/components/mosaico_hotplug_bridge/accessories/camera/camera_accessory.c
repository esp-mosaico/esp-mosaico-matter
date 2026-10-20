/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "mosaico_hotplug_accessory.h"
#include "mosaico_matter_camera.h"

static esp_err_t camera_present(const mosaico_hotplug_event_t *event)
{
    return mosaico_matter_camera_present(event->slot, event->board_id, event->serial_number);
}

static void camera_absent(const mosaico_hotplug_event_t *event)
{
    mosaico_matter_camera_absent(event->slot);
}

const mosaico_hotplug_accessory_t mosaico_hotplug_camera_accessory = {
    .name = "camera",
    .board_type = MOSAICO_BOARD_TYPE_CAMERA,
    .on_present = camera_present,
    .on_absent = camera_absent,
};
