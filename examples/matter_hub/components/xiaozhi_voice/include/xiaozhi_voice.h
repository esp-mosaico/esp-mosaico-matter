/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool started;
    bool wifi_connected;
    bool server_connected;
    bool activating;
    char state[24];
    char transport[16];
    char activation_code[16];
    char activation_message[192];
    char last_error[96];
} xiaozhi_voice_status_t;

/**
 * Start the XiaoZhi voice session on its own task.
 *
 * `matter_manager` is a fabric_manager_handle_t, or NULL when Local Fabric
 * is not running. The call does not draw a UI and does not start Wi-Fi.
 * The Hub must already have brought up wifi_manager and audio_hub.
 */
esp_err_t xiaozhi_voice_start(void *matter_manager);

/** Register the `xiaozhi_talk` console command. */
void xiaozhi_voice_register_console(void);

/** Toggle XiaoZhi listening state. */
void xiaozhi_voice_toggle_chat(void);

/**
 * Playback queue timing for the mic-mute tail after TTS.
 *
 * frame_ms is audio_mixer_config_t.frame_ms. ring_frames is the mixer track
 * ring (AUDIO_MIXER_RING_FRAMES in audio_mixer.c), not the capture subscriber
 * ring. The tail is ring_frames * frame_ms plus a fixed enclosure echo.
 * Call before xiaozhi_voice_start(). Unset values stay at 20 ms and 8 frames.
 * frame_ms must be 1..1000 and ring_frames 1..64.
 */
esp_err_t xiaozhi_voice_set_mixer_timing(uint32_t frame_ms,
                                         uint32_t ring_frames);

/** Read a thread-safe snapshot of the XiaoZhi voice agent state. */
esp_err_t xiaozhi_voice_get_status(xiaozhi_voice_status_t *out);

#ifdef __cplusplus
}
#endif
