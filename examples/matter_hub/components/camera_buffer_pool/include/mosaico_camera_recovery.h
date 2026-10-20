#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mosaico_camera_t;
/* Nonblocking hotplug notification. Sensor/DVP recovery runs under the
 * camera lock on the next open, after consumers return borrowed frames. */
void mosaico_camera_notify_hotplug(struct mosaico_camera_t *camera, bool available);

#ifdef __cplusplus
}
#endif
