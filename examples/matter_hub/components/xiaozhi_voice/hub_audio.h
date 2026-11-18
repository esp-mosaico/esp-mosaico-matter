/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_xiaozhi_chat.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Mixer queue used to keep the mic closed after playback.
 * frame_ms and ring_frames must already be validated.
 * Call before hub_audio_start().
 */
void hub_audio_set_mixer_timing(uint32_t frame_ms, uint32_t ring_frames);

/** Open the audio_hub capture subscriber and the Opus tasks. */
esp_err_t hub_audio_start(esp_xiaozhi_chat_handle_t chat);

/** When true, captured PCM is encoded and sent on the open audio channel. */
void hub_audio_set_uplink(bool enable);

/**
 * Hold the mic closed for a TTS segment. start keeps it closed across gaps
 * between Opus packets. stop releases the hold and leaves the playback tail
 * so audio still in the mixer is not uploaded.
 */
void hub_audio_set_tts_active(bool active);

/**
 * Discard queued speech and stop the mixer on the audio task. Packets already
 * being decoded are invalidated; downlink resumes at the next TTS start.
 */
void hub_audio_stop_playback(void);

/**
 * Queue one Opus packet from the chat audio callback.
 * The buffer is only valid for the duration of that callback.
 */
void hub_audio_on_downlink(const uint8_t *data, int len);

#ifdef __cplusplus
}
#endif
