camera_pool_replace([====[#include "mosaico_camera_service.h"]====] [====[#include "mosaico_camera_service.h"
#include "mosaico_camera_recovery.h"]====] 1)
camera_pool_replace([====[    bool initialized;]====] [====[    bool initialized;
    bool present;]====] 1)
camera_pool_replace([====[        s_service.camera = camera;]====] [====[        s_service.camera = camera;
        s_service.present = true;]====] 1)
camera_pool_replace([====[    if ((event->changes & MOSAICO_MODULE_CHANGE_PRESENCE) && event->info.presence == MOSAICO_MODULE_PRESENCE_ABSENT) {]====] [====[    if (s_service.lock) {
        bool changed = false;
        bool available = event->info.presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
                         event->info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID &&
                         event->info.eeprom.board_type == MOSAICO_BOARD_TYPE_CAMERA;
        xSemaphoreTake(s_service.lock, portMAX_DELAY);
        bool retained = s_service.initialized && s_service.camera;
        if (retained && ((event->changes & MOSAICO_MODULE_CHANGE_PRESENCE) ||
            ((event->changes & MOSAICO_MODULE_CHANGE_DESCRIPTOR) && s_service.present != available))) {
            s_service.present = available;
            mosaico_camera_notify_hotplug(s_service.camera, available);
            changed = true;
        }
        xSemaphoreGive(s_service.lock);
        if (changed) notify_availability('L', available);
        if (retained) return;
    }
    if ((event->changes & MOSAICO_MODULE_CHANGE_PRESENCE) && event->info.presence == MOSAICO_MODULE_PRESENCE_ABSENT) {]====] 1)
camera_pool_replace([====[    if (initialized) {
        *out_camera = s_service.camera;]====] [====[    if (initialized && s_service.present) {
        *out_camera = s_service.camera;]====] 1)
camera_pool_replace([====[    const bool available = s_service.initialized && s_service.camera;]====] [====[    const bool available = s_service.initialized && s_service.present && s_service.camera;]====] 1)
