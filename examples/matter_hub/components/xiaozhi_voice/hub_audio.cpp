/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hub_audio.h"

#include <atomic>
#include <string.h>

#include "audio_hub.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_opus_dec.h"
#include "esp_timer.h"
#include "esp_opus_enc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "hub_wake.h"

#define TAG "xz_audio"
#define SAMPLE_RATE 16000
#define FRAME_MS 60
#define FRAME_SAMPLES ((SAMPLE_RATE * FRAME_MS) / 1000)
/* Encode and decode both sit on this task. */
#define OPUS_STACK_BYTES (48 * 1024)
#define READ_BYTES 640
#define OPUS_SLOT 512
#define OPUS_QUEUE_LEN 20
/* The mixer ring is 160 ms, but packets are decoded as they arrive, so the
 * ring normally holds only the latest 60 ms frame. Hold three frames once
 * per utterance, then decode immediately so the ring stays full. Slack covers
 * a packet that lands just after the 120 ms mark. Rearm only after the ring
 * has drained plus one frame, so a short hole is not held a second time. */
#define PLAYBACK_PREROLL_PACKETS 3
#define PLAYBACK_PREROLL_SLACK_MS 30

typedef struct {
    uint32_t generation;
    uint16_t len;
    uint8_t data[OPUS_SLOT];
} hub_opus_pkt_t;

static esp_xiaozhi_chat_handle_t s_chat;
static audio_capture_sub_handle_t s_capture;
static audio_mixer_track_handle_t s_track;
static void *s_encoder;
static void *s_decoder;
static int s_enc_in_bytes;
static int s_enc_out_bytes;
static int s_dec_rate = SAMPLE_RATE;
static uint8_t s_mixer_channels = 1;
static uint32_t s_mixer_rate = SAMPLE_RATE;
static esp_ae_rate_cvt_handle_t s_resampler;
static int16_t *s_play_pcm;
static int16_t *s_play_stereo;
static StreamBufferHandle_t s_pcm;
static QueueHandle_t s_downlink;
static TaskHandle_t s_reader;
static TaskHandle_t s_opus;
static StaticStreamBuffer_t *s_pcm_ctrl;
static uint8_t *s_pcm_storage;
static StaticQueue_t *s_downlink_ctrl;
static uint8_t *s_downlink_storage;
static std::atomic<bool> s_uplink{false};
static std::atomic<bool> s_opus_stop{false};
static TaskHandle_t s_opus_join;
/* While TTS is active the mic stays closed, including gaps between Opus
 * packets. stop ends that hold. Audio already queued still has to finish:
 * the mixer ring, then the codec and the enclosure echo. There is no drain
 * event, so that remainder is a fixed tail. Defaults match audio_hub. */
static uint32_t s_mixer_frame_ms = 20;
static uint32_t s_mixer_ring_frames = 8;
static constexpr int64_t kPlaybackEchoUs = 150 * 1000;
static std::atomic<int64_t> s_play_mute_until_us{0};
static std::atomic<bool> s_play_muting{false};
static std::atomic<bool> s_tts_hold{false};
/* Only the Opus task writes/stops playback. Tag packets so cancellation also
 * invalidates a packet already dequeued or being decoded. */
static portMUX_TYPE s_playback_mux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<uint32_t> s_playback_generation{0};
static std::atomic<uint32_t> s_flushed_generation{0};
static std::atomic<bool> s_accept_downlink{true};

static int64_t playback_tail_us(void)
{
    return static_cast<int64_t>(s_mixer_ring_frames) * static_cast<int64_t>(s_mixer_frame_ms) * 1000 +
           kPlaybackEchoUs;
}

static int64_t preroll_wait_us(void)
{
    return static_cast<int64_t>(PLAYBACK_PREROLL_PACKETS - 1) * FRAME_MS * 1000 +
           static_cast<int64_t>(PLAYBACK_PREROLL_SLACK_MS) * 1000;
}

static int64_t preroll_rearm_us(void)
{
    return static_cast<int64_t>(s_mixer_ring_frames) * static_cast<int64_t>(s_mixer_frame_ms) * 1000 +
           static_cast<int64_t>(FRAME_MS) * 1000;
}

void hub_audio_set_mixer_timing(uint32_t frame_ms, uint32_t ring_frames)
{
    s_mixer_frame_ms = frame_ms;
    s_mixer_ring_frames = ring_frames;
}

static void note_playback(void)
{
    s_play_mute_until_us.store(esp_timer_get_time() + playback_tail_us(), std::memory_order_relaxed);
    if (!s_play_muting.exchange(true, std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "mute mic while speaker plays");
    }
}

static bool mic_is_muted(void)
{
    portENTER_CRITICAL(&s_playback_mux);
    const bool held = s_tts_hold.load(std::memory_order_relaxed) ||
        s_playback_generation.load(std::memory_order_acquire) !=
        s_flushed_generation.load(std::memory_order_acquire);
    portEXIT_CRITICAL(&s_playback_mux);
    if (held) {
        return true;
    }
    return esp_timer_get_time() < s_play_mute_until_us.load(std::memory_order_relaxed);
}

void hub_audio_set_tts_active(bool active)
{
    if (active) {
        portENTER_CRITICAL(&s_playback_mux);
        s_tts_hold.store(true, std::memory_order_relaxed);
        s_accept_downlink.store(true, std::memory_order_release);
        portEXIT_CRITICAL(&s_playback_mux);
        note_playback();
        return;
    }
    portENTER_CRITICAL(&s_playback_mux);
    const bool was_held = s_tts_hold.exchange(false, std::memory_order_relaxed);
    portEXIT_CRITICAL(&s_playback_mux);
    if (!was_held && !s_play_muting.load(std::memory_order_relaxed)) {
        return;
    }
    const int64_t until = esp_timer_get_time() + playback_tail_us();
    int64_t prev = s_play_mute_until_us.load(std::memory_order_relaxed);
    while (prev < until &&
           !s_play_mute_until_us.compare_exchange_weak(prev, until, std::memory_order_relaxed)) {
    }
}

void hub_audio_stop_playback(void)
{
    portENTER_CRITICAL(&s_playback_mux);
    s_accept_downlink.store(false, std::memory_order_release);
    s_playback_generation.fetch_add(1, std::memory_order_release);
    s_tts_hold.store(false, std::memory_order_relaxed);
    portEXIT_CRITICAL(&s_playback_mux);
    if (s_opus != nullptr) {
        xTaskNotifyGive(s_opus);
    }
}

static uint8_t *s_enc_buf;
static int16_t *s_dec_pcm;
static int s_send_errors;
static int s_decode_errors;

static int open_decoder(int rate)
{
    if (s_decoder != nullptr) {
        esp_opus_dec_close(s_decoder);
        s_decoder = nullptr;
    }
    esp_opus_dec_cfg_t cfg = {};
    cfg.sample_rate = static_cast<uint32_t>(rate);
    cfg.channel = 1;
    cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    cfg.self_delimited = false;
    esp_audio_err_t err = esp_opus_dec_open(&cfg, sizeof(cfg), &s_decoder);
    if (err != ESP_AUDIO_ERR_OK || s_decoder == nullptr) {
        s_decoder = nullptr;
        ESP_LOGE(TAG, "opus decoder open failed at %d Hz", rate);
        return -1;
    }
    s_dec_rate = rate;
    ESP_LOGI(TAG, "opus decoder %d Hz", rate);
    return 0;
}

static void ensure_resampler(int src_rate)
{
    if (src_rate == static_cast<int>(s_mixer_rate)) {
        return;
    }
    if (s_resampler != nullptr) {
        return;
    }
    esp_ae_rate_cvt_cfg_t cfg = {};
    cfg.src_rate = static_cast<uint32_t>(src_rate);
    cfg.dest_rate = s_mixer_rate;
    cfg.channel = 1;
    cfg.bits_per_sample = 16;
    cfg.complexity = 2;
    cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_MEMORY;
    if (esp_ae_rate_cvt_open(&cfg, &s_resampler) != ESP_AE_ERR_OK) {
        s_resampler = nullptr;
        ESP_LOGW(TAG, "resampler %d -> %u was not created", src_rate, (unsigned)s_mixer_rate);
    } else {
        ESP_LOGI(TAG, "resample playback %d -> %u", src_rate, (unsigned)s_mixer_rate);
    }
}

static void write_track(const int16_t *pcm, int samples, uint32_t generation)
{
    if (s_track == nullptr || pcm == nullptr || samples <= 0) {
        return;
    }
    const int16_t *src = pcm;
    size_t bytes = static_cast<size_t>(samples) * sizeof(int16_t);
    if (s_mixer_channels == 2) {
        if (s_play_stereo == nullptr) {
            return;
        }
        for (int i = 0; i < samples; ++i) {
            s_play_stereo[i * 2] = pcm[i];
            s_play_stereo[i * 2 + 1] = pcm[i];
        }
        src = s_play_stereo;
        bytes = static_cast<size_t>(samples) * 2 * sizeof(int16_t);
    }
    if (generation != s_playback_generation.load(std::memory_order_acquire)) {
        return;
    }
    audio_mixer_track_write(s_track, src, bytes);
    note_playback();
}

static void play_decoded(int16_t *pcm, int samples, uint32_t generation)
{
    if (s_dec_rate == static_cast<int>(s_mixer_rate) || s_resampler == nullptr) {
        write_track(pcm, samples, generation);
        return;
    }
    uint32_t out_cap = 0;
    if (esp_ae_rate_cvt_get_max_out_sample_num(s_resampler, static_cast<uint32_t>(samples),
                                               &out_cap) != ESP_AE_ERR_OK ||
        out_cap == 0 || s_play_pcm == nullptr) {
        return;
    }
    uint32_t produced = out_cap;
    esp_ae_err_t err = esp_ae_rate_cvt_process(s_resampler, static_cast<esp_ae_sample_t>(pcm),
                                               static_cast<uint32_t>(samples),
                                               static_cast<esp_ae_sample_t>(s_play_pcm), &produced);
    if (err == ESP_AE_ERR_OK && produced > 0) {
        write_track(s_play_pcm, static_cast<int>(produced), generation);
    }
}

static int decode_packet(const hub_opus_pkt_t *pkt, int16_t *pcm, int pcm_samples)
{
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = const_cast<uint8_t *>(pkt->data);
    raw.len = pkt->len;
    raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;
    esp_audio_dec_out_frame_t out = {};
    out.buffer = reinterpret_cast<uint8_t *>(pcm);
    out.len = static_cast<uint32_t>(pcm_samples * static_cast<int>(sizeof(int16_t)));
    esp_audio_dec_info_t info = {};
    esp_audio_err_t err = esp_opus_dec_decode(s_decoder, &raw, &out, &info);
    if (err != ESP_AUDIO_ERR_OK || out.decoded_size < sizeof(int16_t)) {
        return -1;
    }
    return static_cast<int>(out.decoded_size / sizeof(int16_t));
}

static void decode_and_play(const hub_opus_pkt_t *pkt)
{
    constexpr int kMaxSamples = 48000 * 120 / 1000;
    if (s_dec_pcm == nullptr) {
        return;
    }
    int samples = decode_packet(pkt, s_dec_pcm, kMaxSamples);
    if (samples < 0 && s_dec_rate != 24000) {
        ESP_LOGW(TAG, "16 kHz decode failed, retrying at 24000 Hz");
        if (open_decoder(24000) == 0) {
            ensure_resampler(24000);
            samples = decode_packet(pkt, s_dec_pcm, kMaxSamples);
        }
    }
    if (samples > 0) {
        s_decode_errors = 0;
        play_decoded(s_dec_pcm, samples, pkt->generation);
    } else if (++s_decode_errors == 1 || (s_decode_errors % 50) == 0) {
        ESP_LOGW(TAG, "opus decode failed, count=%d", s_decode_errors);
    }
}

static void encode_and_send(const int16_t *pcm)
{
    if (s_encoder == nullptr || s_chat == 0 || !s_uplink.load() || mic_is_muted()) {
        return;
    }
    if (s_enc_buf == nullptr) {
        return;
    }
    esp_audio_enc_in_frame_t in = {};
    in.buffer = reinterpret_cast<uint8_t *>(const_cast<int16_t *>(pcm));
    in.len = static_cast<uint32_t>(s_enc_in_bytes);
    esp_audio_enc_out_frame_t encoded = {};
    encoded.buffer = s_enc_buf;
    encoded.len = static_cast<uint32_t>(s_enc_out_bytes);
    esp_audio_err_t err = esp_opus_enc_process(s_encoder, &in, &encoded);
    if (err == ESP_AUDIO_ERR_OK && encoded.encoded_bytes > 0 && s_uplink.load() && !mic_is_muted()) {
        esp_err_t send = esp_xiaozhi_chat_send_audio_data(
            s_chat, reinterpret_cast<char *>(s_enc_buf), encoded.encoded_bytes);
        if (send == ESP_OK) {
            s_send_errors = 0;
        } else if (++s_send_errors == 1 || (s_send_errors % 50) == 0) {
            ESP_LOGW(TAG, "send audio: %s (count=%d)", esp_err_to_name(send), s_send_errors);
        }
    }
}

static void opus_task(void *arg)
{
    (void)arg;
    int16_t *pcm = static_cast<int16_t *>(
        heap_caps_malloc(FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pcm == nullptr) {
        ESP_LOGE(TAG, "opus pcm buffer was not allocated");
        if (s_opus_join != nullptr) {
            xTaskNotifyGive(s_opus_join);
        }
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    const size_t frame_bytes = FRAME_SAMPLES * sizeof(int16_t);
    bool preroll = true;
    int64_t preroll_since_us = 0;
    int64_t last_played_us = 0;
    while (!s_opus_stop.load(std::memory_order_acquire)) {
        const uint32_t generation = s_playback_generation.load(std::memory_order_acquire);
        if (generation != s_flushed_generation.load(std::memory_order_relaxed)) {
            audio_mixer_track_stop(s_track);
            hub_opus_pkt_t stale = {};
            while (xQueuePeek(s_downlink, &stale, 0) == pdTRUE &&
                   stale.generation != generation) {
                xQueueReceive(s_downlink, &stale, 0);
            }
            // The ring is empty; only the codec/enclosure echo remains.
            s_play_mute_until_us.store(esp_timer_get_time() + kPlaybackEchoUs,
                                      std::memory_order_relaxed);
            s_flushed_generation.store(generation, std::memory_order_release);
            preroll = true;
            preroll_since_us = 0;
            last_played_us = 0;
        }
        const int64_t now = esp_timer_get_time();
        const UBaseType_t waiting = s_downlink == nullptr ? 0 : uxQueueMessagesWaiting(s_downlink);
        if (waiting == 0) {
            preroll_since_us = 0;
            if (!preroll && last_played_us != 0 && now - last_played_us >= preroll_rearm_us()) {
                preroll = true;
            }
        }
        bool hold = false;
        if (preroll && waiting > 0 && waiting < PLAYBACK_PREROLL_PACKETS) {
            if (preroll_since_us == 0) {
                preroll_since_us = now;
            }
            hold = now - preroll_since_us < preroll_wait_us();
        }
        if (!hold && waiting > 0) {
            hub_opus_pkt_t pkt = {};
            if (xQueueReceive(s_downlink, &pkt, 0) == pdTRUE) {
                if (pkt.generation != s_playback_generation.load(std::memory_order_acquire)) {
                    continue;
                }
                if (preroll) {
                    ESP_LOGD(TAG, "playback buffered %u packets", static_cast<unsigned>(waiting));
                    preroll = false;
                    preroll_since_us = 0;
                }
                last_played_us = esp_timer_get_time();
                decode_and_play(&pkt);
                continue;
            }
        }
        const bool send_mic = s_uplink.load() && !mic_is_muted();
        if (send_mic && s_pcm != nullptr && xStreamBufferBytesAvailable(s_pcm) >= frame_bytes) {
            size_t got = xStreamBufferReceive(s_pcm, pcm, frame_bytes, 0);
            if (got == frame_bytes) {
                encode_and_send(pcm);
                continue;
            }
        } else if (!send_mic && s_pcm != nullptr) {
            uint8_t dump[64];
            while (xStreamBufferReceive(s_pcm, dump, sizeof(dump), 0) > 0) {
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
    free(pcm);
    if (s_opus_join != nullptr) {
        xTaskNotifyGive(s_opus_join);
    }
    vTaskDeleteWithCaps(nullptr);
}

static void reader_task(void *arg)
{
    (void)arg;
    uint8_t buf[READ_BYTES];
    while (true) {
        size_t n = audio_capture_sub_read(s_capture, buf, sizeof(buf), 100);
        if (n < sizeof(int16_t)) {
            continue;
        }
        n -= n % sizeof(int16_t);
        hub_wake_feed(reinterpret_cast<int16_t *>(buf), n / sizeof(int16_t));
        if (mic_is_muted()) {
            continue;
        }
        if (s_play_muting.exchange(false, std::memory_order_relaxed)) {
            ESP_LOGI(TAG, "mic unmuted");
        }
        if (s_uplink.load() && s_pcm != nullptr) {
            xStreamBufferSend(s_pcm, buf, n, 0);
            if (s_opus != nullptr) {
                xTaskNotifyGive(s_opus);
            }
        }
    }
}

static esp_err_t open_capture(void)
{
    audio_capture_handle_t capture = nullptr;
    esp_err_t err = audio_hub_get_capture(&capture);
    if (err != ESP_OK || capture == nullptr) {
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    const audio_capture_sub_format_t format = {
        .sample_rate = SAMPLE_RATE,
        .channels = 1,
        .bits = 16,
    };
    return audio_capture_open_subscriber(capture, AUDIO_CAPTURE_SUB_APP, &format, "xiaozhi/capture",
                                         &s_capture);
}

static esp_err_t open_track(void)
{
    audio_mixer_handle_t mixer = nullptr;
    esp_err_t err = audio_hub_get_mixer(&mixer);
    if (err != ESP_OK || mixer == nullptr) {
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    err = audio_mixer_open_track(mixer, AUDIO_MIXER_TRACK_APP, "xiaozhi/playback", &s_track);
    if (err != ESP_OK) {
        s_track = nullptr;
        return err;
    }
    uint8_t bits = 16;
    audio_mixer_track_info(s_track, &s_mixer_rate, &s_mixer_channels, &bits);
    if (s_mixer_channels == 0) {
        s_mixer_channels = 1;
    }
    ESP_LOGI(TAG, "playback mixer %u Hz, %u ch", (unsigned)s_mixer_rate, s_mixer_channels);
    if (s_mixer_channels == 2) {
        s_play_stereo = static_cast<int16_t *>(
            heap_caps_malloc(48000 * 120 / 1000 * 2 * sizeof(int16_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    s_play_pcm = static_cast<int16_t *>(heap_caps_malloc(
        48000 * 120 / 1000 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_mixer_rate != SAMPLE_RATE) {
        ensure_resampler(SAMPLE_RATE);
    }
    return ESP_OK;
}

static void audio_fail_cleanup(void)
{
    if (s_opus != nullptr) {
        s_opus_join = xTaskGetCurrentTaskHandle();
        s_opus_stop.store(true, std::memory_order_release);
        xTaskNotifyGive(s_opus);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        s_opus = nullptr;
        s_opus_join = nullptr;
    }
    if (s_capture != nullptr) {
        audio_capture_close_subscriber(s_capture);
        s_capture = nullptr;
    }
    if (s_track != nullptr) {
        audio_mixer_close_track(s_track);
        s_track = nullptr;
    }
    if (s_resampler != nullptr) {
        esp_ae_rate_cvt_close(s_resampler);
        s_resampler = nullptr;
    }
    if (s_encoder != nullptr) {
        esp_opus_enc_close(s_encoder);
        s_encoder = nullptr;
    }
    if (s_decoder != nullptr) {
        esp_opus_dec_close(s_decoder);
        s_decoder = nullptr;
    }
    if (s_pcm != nullptr) {
        vStreamBufferDelete(s_pcm);
        s_pcm = nullptr;
    }
    if (s_downlink != nullptr) {
        vQueueDelete(s_downlink);
        s_downlink = nullptr;
    }
    free(s_pcm_ctrl);
    s_pcm_ctrl = nullptr;
    free(s_pcm_storage);
    s_pcm_storage = nullptr;
    free(s_downlink_ctrl);
    s_downlink_ctrl = nullptr;
    free(s_downlink_storage);
    s_downlink_storage = nullptr;
    free(s_enc_buf);
    s_enc_buf = nullptr;
    free(s_dec_pcm);
    s_dec_pcm = nullptr;
    free(s_play_pcm);
    s_play_pcm = nullptr;
    free(s_play_stereo);
    s_play_stereo = nullptr;
}

esp_err_t hub_audio_start(esp_xiaozhi_chat_handle_t chat)
{
    if (chat == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_chat = chat;
    esp_err_t err = open_capture();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "capture subscriber was not opened: %s", esp_err_to_name(err));
        return err;
    }
    err = open_track();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "playback track was not opened: %s", esp_err_to_name(err));
        audio_fail_cleanup();
        return err;
    }

    esp_opus_enc_config_t enc_cfg = {};
    enc_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
    enc_cfg.channel = ESP_AUDIO_MONO;
    enc_cfg.bits_per_sample = ESP_AUDIO_BIT16;
    enc_cfg.bitrate = 24000;
    enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    enc_cfg.complexity = 0;
    enc_cfg.enable_fec = false;
    enc_cfg.enable_dtx = false;
    enc_cfg.enable_vbr = false;
    if (esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &s_encoder) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "opus encoder was not created");
        audio_fail_cleanup();
        return ESP_FAIL;
    }
    int in_size = 0;
    const int frame_bytes = FRAME_SAMPLES * static_cast<int>(sizeof(int16_t));
    if (esp_opus_enc_get_frame_size(s_encoder, &in_size, &s_enc_out_bytes) != ESP_AUDIO_ERR_OK ||
        in_size <= 0 || s_enc_out_bytes <= 0) {
        ESP_LOGE(TAG, "opus frame size is unavailable");
        audio_fail_cleanup();
        return ESP_FAIL;
    }
    if (in_size == frame_bytes) {
        s_enc_in_bytes = in_size;
    } else if (in_size == FRAME_SAMPLES) {
        s_enc_in_bytes = frame_bytes;
    } else {
        ESP_LOGW(TAG, "opus frame size %d is unexpected; using %d bytes", in_size, frame_bytes);
        s_enc_in_bytes = frame_bytes;
    }
    if (s_enc_out_bytes < 1024) {
        s_enc_out_bytes = 1024;
    }
    s_enc_buf = static_cast<uint8_t *>(
        heap_caps_malloc(static_cast<size_t>(s_enc_out_bytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_dec_pcm = static_cast<int16_t *>(heap_caps_malloc(48000 * 120 / 1000 * sizeof(int16_t),
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_enc_buf == nullptr || s_dec_pcm == nullptr) {
        audio_fail_cleanup();
        return ESP_ERR_NO_MEM;
    }
    if (open_decoder(SAMPLE_RATE) != 0) {
        audio_fail_cleanup();
        return ESP_FAIL;
    }

    constexpr size_t pcm_bytes = FRAME_SAMPLES * sizeof(int16_t) * 8;
    s_pcm_ctrl = static_cast<StaticStreamBuffer_t *>(
        heap_caps_malloc(sizeof(StaticStreamBuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_pcm_storage =
        static_cast<uint8_t *>(heap_caps_malloc(pcm_bytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_downlink_ctrl = static_cast<StaticQueue_t *>(
        heap_caps_malloc(sizeof(StaticQueue_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_downlink_storage = static_cast<uint8_t *>(heap_caps_malloc(
        OPUS_QUEUE_LEN * sizeof(hub_opus_pkt_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_pcm_ctrl != nullptr && s_pcm_storage != nullptr) {
        s_pcm = xStreamBufferCreateStatic(pcm_bytes, frame_bytes, s_pcm_storage, s_pcm_ctrl);
    }
    if (s_downlink_ctrl != nullptr && s_downlink_storage != nullptr) {
        s_downlink = xQueueCreateStatic(OPUS_QUEUE_LEN, sizeof(hub_opus_pkt_t), s_downlink_storage,
                                        s_downlink_ctrl);
    }
    if (s_pcm == nullptr || s_downlink == nullptr) {
        ESP_LOGE(TAG, "audio queues were not allocated");
        audio_fail_cleanup();
        return ESP_ERR_NO_MEM;
    }

    s_opus_stop.store(false, std::memory_order_relaxed);
    BaseType_t opus_ok = xTaskCreatePinnedToCoreWithCaps(
        opus_task, "xz_opus", OPUS_STACK_BYTES, nullptr, 4, &s_opus, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (opus_ok != pdPASS) {
        s_opus = nullptr;
        ESP_LOGE(TAG, "opus task was not created");
        audio_fail_cleanup();
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(reader_task, "xz_mic", 4096, nullptr, 8, &s_reader, 0) != pdPASS) {
        s_reader = nullptr;
        ESP_LOGE(TAG, "mic reader task was not created");
        audio_fail_cleanup();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "audio path ready, opus frame %d ms, mute tail %u ms, preroll %d packets", FRAME_MS,
             (unsigned)(playback_tail_us() / 1000), PLAYBACK_PREROLL_PACKETS);
    return ESP_OK;
}

void hub_audio_set_uplink(bool enable)
{
    s_uplink.store(enable);
    if (!enable) {
        portENTER_CRITICAL(&s_playback_mux);
        s_tts_hold.store(false, std::memory_order_relaxed);
        portEXIT_CRITICAL(&s_playback_mux);
        return;
    }
    if (s_opus != nullptr) {
        xTaskNotifyGive(s_opus);
    }
}

void hub_audio_on_downlink(const uint8_t *data, int len)
{
    if (data == nullptr || len <= 0 || s_downlink == nullptr) {
        return;
    }
    if (len > OPUS_SLOT) {
        ESP_LOGW(TAG, "drop opus packet of %d bytes", len);
        return;
    }
    hub_opus_pkt_t pkt = {};
    pkt.generation = s_playback_generation.load(std::memory_order_acquire);
    if (!s_accept_downlink.load(std::memory_order_acquire)) {
        return;
    }
    pkt.len = static_cast<uint16_t>(len);
    memcpy(pkt.data, data, static_cast<size_t>(len));
    if (xQueueSend(s_downlink, &pkt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "playback queue full, drop %d bytes", len);
        return;
    }
    if (s_opus != nullptr) {
        xTaskNotifyGive(s_opus);
    }
}
