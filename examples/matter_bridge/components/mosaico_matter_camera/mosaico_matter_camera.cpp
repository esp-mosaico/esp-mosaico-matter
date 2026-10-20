/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "mosaico_matter_camera.h"
#include "mock_media.h"
#include "sdp_compat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <new>
#include <deque>
#include <string>
#include <vector>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_matter.h"
#include "esp_matter_cluster.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_timer.h"
#include "bridge_dev_mngr.h"
#include "mosaico_module_camera.h"
#include "encoder/esp_video_enc.h"
#include "encoder/esp_video_enc_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include <app/CASESessionManager.h>
#include <app/server/Server.h>
#include <controller/InvokeInteraction.h>
#include <data_model_provider/clusters/camera_av_stream_management/integration.h>
#include <data_model_provider/clusters/webrtc_transport_provider/integration.h>
#include <platform/CHIPDeviceLayer.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::CameraAvStreamManagement;
using namespace chip::app::Clusters::WebRTCTransportProvider;
using chip::Protocols::InteractionModel::Status;
using namespace esp_matter;

namespace {

constexpr uint16_t kHdCaptureWidth = 1280;
constexpr uint16_t kHdCaptureHeight = 720;
constexpr uint16_t kVgaCaptureWidth = 640;
constexpr uint16_t kVgaCaptureHeight = 480;
constexpr uint16_t kWidth = 320;
constexpr uint16_t kHeight = 240;
static_assert(kHdCaptureHeight == kHeight * 3);
static_assert(kVgaCaptureHeight == kHeight * 2);
static_assert((kHdCaptureWidth - kWidth * 3) % 4 == 0);
constexpr uint16_t kFramesPerSecond = 10;
constexpr uint32_t kCaptureTimeoutMs = 500;
constexpr uint32_t kDefaultBitrate = 320000;
constexpr uint32_t kMaxBitrate = 768000;
constexpr uint32_t kRawFrameSize = kWidth * kHeight * 2;
constexpr uint32_t kEncodedBufferSize = 128 * 1024;
constexpr uint32_t kCameraDeviceType = 0x0142;
constexpr char kCameraProfileId[] = "mosaico_camera";
constexpr uint16_t kVideoStreamId = 1;
constexpr uint16_t kAudioStreamId = 1;
constexpr uint16_t kSnapshotStreamId = 1;
constexpr uint32_t kMaxNetworkBandwidth = kMaxBitrate + mosaico_camera_mock::kMaxAudioBitRate;
std::array<AudioCodecEnum, 1> sAudioCodecs{ AudioCodecEnum::kOpus };
std::array<uint32_t, 1> sAudioSampleRates{ mosaico_camera_mock::kAudioSampleRate };
auto sAudioBitDepths = mosaico_camera_mock::kAudioBitDepths;
constexpr size_t kMaxIceServers = 10;
// Leave room for Matter Invoke/TLV headers and the security tag in one MRP packet.
constexpr size_t kMaxSmallSignalBytes = 1000;
const char *TAG = "matter_camera";

struct OutboundMessage {
    enum class Type { Offer, Answer, Candidate } type;
    uint16_t session_id;
    std::string payload;
};

struct DevMngrWait {
    SemaphoreHandle_t done;
    std::atomic<unsigned> references{2};
    bridge_dev_mngr_request_result_t result{};
};

void DevMngrCompleted(bridge_dev_mngr_handle_t, const bridge_dev_mngr_request_result_t *result,
                   void *ctx)
{
    auto *wait = static_cast<DevMngrWait *>(ctx);
    wait->result = *result;
    xSemaphoreGive(wait->done);
    if (wait->references.fetch_sub(1) == 1) {
        vSemaphoreDelete(wait->done);
        delete wait;
    }
}

DevMngrWait *NewDevMngrWait()
{
    auto *wait = new (std::nothrow) DevMngrWait;
    if (!wait) return nullptr;
    wait->done = xSemaphoreCreateBinary();
    if (!wait->done) {
        delete wait;
        return nullptr;
    }
    return wait;
}

esp_err_t FinishDevMngrWait(DevMngrWait *wait, esp_err_t error,
                         bridge_dev_mngr_device_info_t *result = nullptr)
{
    if (error != ESP_OK) {
        if (wait->references.fetch_sub(1) == 1) {
            vSemaphoreDelete(wait->done);
            delete wait;
        }
    } else if (xSemaphoreTake(wait->done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        error = ESP_ERR_TIMEOUT;
    } else {
        error = wait->result.error;
        if (error == ESP_OK && result) *result = wait->result.device;
    }
    if (wait->references.fetch_sub(1) == 1) {
        vSemaphoreDelete(wait->done);
        delete wait;
    }
    return error;
}

class MosaicoMatterCamera final : public CameraAVStreamManagementDelegate,
                                  public WebRTCTransportProvider::Delegate {
public:
    MosaicoMatterCamera() : mOnConnected(OnDeviceConnected, this),
                            mOnConnectionFailure(OnDeviceConnectionFailure, this) {}

    esp_err_t Init(bridge_dev_mngr_handle_t dev_mngr);
    esp_err_t Present(mosaico_module_mgr_slot_t slot, uint16_t board_id, uint32_t serial);
    void Absent(mosaico_module_mgr_slot_t slot);

    Status VideoStreamAllocate(const VideoStreamStruct & args, uint16_t & out_id) override;
    void OnVideoStreamAllocated(const VideoStreamStruct & stream, StreamAllocationAction action) override;
    Status VideoStreamModify(uint16_t stream_id, Optional<bool>, Optional<bool>) override;
    Status VideoStreamDeallocate(uint16_t stream_id) override;
    Status AudioStreamAllocate(const AudioStreamStruct &, uint16_t &) override;
    Status AudioStreamDeallocate(uint16_t) override;
    Status SnapshotStreamAllocate(const SnapshotStreamAllocateArgs &, uint16_t &) override;
    Status SnapshotStreamModify(uint16_t, Optional<bool>, Optional<bool>) override { return Status::UnsupportedCommand; }
    Status SnapshotStreamDeallocate(uint16_t) override;
    void OnStreamUsagePrioritiesChanged() override {}
    void OnAttributeChanged(AttributeId) override {}
    Status CaptureSnapshot(DataModel::Nullable<uint16_t>, const VideoResolutionStruct &, ImageSnapshot &) override;
    CHIP_ERROR PersistentAttributesLoadedCallback() override;
    const std::vector<VideoStreamStruct> & GetAllocatedVideoStreams() const override;
    const std::vector<AudioStreamStruct> & GetAllocatedAudioStreams() const override;

    CHIP_ERROR HandleSolicitOffer(const OfferRequestArgs &, WebRTCSessionStruct &, bool &) override;
    CHIP_ERROR HandleProvideOffer(const ProvideOfferRequestArgs &, WebRTCSessionStruct &) override;
    CHIP_ERROR HandleProvideAnswer(uint16_t, const std::string &) override;
    CHIP_ERROR HandleProvideICECandidates(uint16_t, const std::vector<ICECandidateStruct> &) override;
    CHIP_ERROR HandleEndSession(uint16_t, WebRTCEndReasonEnum) override;
    CHIP_ERROR ValidateStreamUsage(StreamUsageEnum, Optional<std::vector<uint16_t>> &,
                                   Optional<std::vector<uint16_t>> &) override;
    CHIP_ERROR ValidateVideoStreamID(uint16_t id) override;
    CHIP_ERROR ValidateAudioStreamID(uint16_t) override;
    CHIP_ERROR ValidateVideoStreams(const std::vector<uint16_t> &) override;
    CHIP_ERROR ValidateAudioStreams(const std::vector<uint16_t> &) override;
    CHIP_ERROR IsStreamUsageSupported(StreamUsageEnum usage) override
    { return usage == StreamUsageEnum::kLiveView ? CHIP_NO_ERROR : CHIP_ERROR_NOT_FOUND; }
    CHIP_ERROR IsHardPrivacyModeActive(bool & active) override { active = false; return CHIP_NO_ERROR; }
    CHIP_ERROR IsSoftRecordingPrivacyModeActive(bool & active) override { active = false; return CHIP_NO_ERROR; }
    CHIP_ERROR IsSoftLivestreamPrivacyModeActive(bool & active) override { active = false; return CHIP_NO_ERROR; }
    bool HasAllocatedVideoStreams() override { return !GetAllocatedVideoStreams().empty(); }
    bool HasAllocatedAudioStreams() override { return !GetAllocatedAudioStreams().empty(); }
    CHIP_ERROR ValidateSFrameConfig(uint16_t, size_t) override { return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE; }
    CHIP_ERROR IsUTCTimeNull(bool & is_null) override { is_null = false; return CHIP_NO_ERROR; }

private:
    static esp_err_t BuildEndpoint(void *endpoint, const char *device_id,
                                   void *ctx);
    static void FormatDevice(uint16_t board_id, uint32_t serial,
                             bridge_dev_mngr_device_definition_t &device);
    esp_err_t Register(bridge_dev_mngr_device_info_t &result);
    esp_err_t Activate(bridge_dev_mngr_device_info_t &result);
    esp_err_t SetReachable(bool reachable,
                           bridge_dev_mngr_device_info_t *result = nullptr);
    esp_err_t Deactivate();
    esp_err_t Remove();
    esp_err_t ConfigureEndpoint(endpoint_t *endpoint);
    const std::vector<SnapshotStreamStruct> & GetAllocatedSnapshotStreams() const;
    void FillSession(const OfferRequestArgs &, WebRTCSessionStruct &);
    CHIP_ERROR StartPeer(const OfferRequestArgs &, bool initiator, const std::string *remote_offer);
    bool StopPeer();
    esp_err_t OpenCamera(mosaico_module_mgr_slot_t slot);
    esp_err_t ReleaseCamera();
    void CopyIceServers(const Optional<std::vector<ICEServerDecodableStruct>> &);
    CHIP_ERROR SendRemoteMessage(esp_peer_msg_type_t type, const std::string &text);
    CHIP_ERROR BuildSdpWithCandidates(const std::string &base,
                                      const std::vector<ICECandidateStruct> &candidates,
                                      std::string &result);
    static int PeerState(esp_peer_state_t state, void *ctx);
    static int PeerMessage(esp_peer_msg_t *message, void *ctx);
    static void PeerTask(void *ctx);
    static void VideoTask(void *ctx);
    bool StartVideo();
    void StopVideo();
    void QueueOutbound(OutboundMessage::Type, uint16_t, const uint8_t *, size_t);
    static void DeliverOutbound(intptr_t arg);
    void PumpOutbound();
    CHIP_ERROR SendFront(Messaging::ExchangeManager &, const SessionHandle &);
    void OutboundDone(bool success);
    static void OnDeviceConnected(void *, Messaging::ExchangeManager &, const SessionHandle &);
    static void OnDeviceConnectionFailure(void *, const ScopedNodeId &, CHIP_ERROR);

    bridge_dev_mngr_handle_t mDevMngr = nullptr;
    bridge_dev_mngr_profile_t mProfile{};
    bridge_dev_mngr_device_definition_t mDevice{};
    mosaico_camera_handle_t mCamera = nullptr;
    uint16_t mCaptureWidth = 0;
    uint16_t mCaptureHeight = 0;
    mosaico_module_mgr_slot_t mSlot = MOSAICO_MODULE_MGR_SLOT_AUTO;
    uint16_t mBoardId = 0;
    uint32_t mSerial = 0;
    uint16_t mEndpointId = 0;
    std::atomic<bool> mPresent{false};

    std::vector<VideoStreamStruct> mEmptyVideo;
    std::vector<AudioStreamStruct> mEmptyAudio;
    std::vector<SnapshotStreamStruct> mEmptySnapshots;
    VideoStreamStruct mVideoConfig{};
    std::atomic<bool> mVideoAllocated{false};

    esp_peer_handle_t mPeer = nullptr;
    std::atomic<bool> mPeerRun{false};
    std::atomic<bool> mPeerStopped{true};
    std::atomic<bool> mPeerConnected{false};
    std::atomic<bool> mVideoRun{false};
    std::atomic<bool> mVideoStopped{true};
    bool mInitiator = false;
    bool mSessionVideo = false;
    bool mSessionAudio = false;
    uint32_t mSessionAudioBitRate = mosaico_camera_mock::kAudioBitRate;
    bool mAnswerQueued = false;
    uint16_t mSessionId = 0;
    std::string mPendingAnswer;
    std::string mRemoteOffer;
    std::vector<mosaico_camera_sdp::OfferedMedia> mOfferedMedia;
    ScopedNodeId mPeerNode;
    EndpointId mPeerEndpoint = 0;
    esp_peer_ice_server_cfg_t mIceServers[kMaxIceServers]{};
    char mIceUrls[kMaxIceServers][2001]{};
    char mIceUsers[kMaxIceServers][509]{};
    char mIcePasswords[kMaxIceServers][513]{};
    size_t mIceServerCount = 0;

    esp_video_enc_handle_t mEncoder = nullptr;
    bool mCodecRegistered = false;
    uint8_t *mRawFrame = nullptr;
    uint8_t *mEncodedFrame = nullptr;

    std::deque<OutboundMessage> mOutbound;
    bool mOutboundBusy = false;
    uint8_t mOutboundRetries = 0;
    Callback::Callback<chip::OnDeviceConnected> mOnConnected;
    Callback::Callback<chip::OnDeviceConnectionFailure> mOnConnectionFailure;
};

MosaicoMatterCamera sCamera;

void MosaicoMatterCamera::FormatDevice(uint16_t board_id, uint32_t serial,
                                       bridge_dev_mngr_device_definition_t &device)
{
    memset(&device, 0, sizeof(device));
    snprintf(device.id, sizeof(device.id), "mosaico-camera-%04x%08lx", board_id,
             static_cast<unsigned long>(serial));
    snprintf(device.name, sizeof(device.name), "Mosaico Camera %04x-%08lx", board_id,
             static_cast<unsigned long>(serial));
    snprintf(device.profile_id, sizeof(device.profile_id), "%s",
             kCameraProfileId);
    device.device_type = kCameraDeviceType;
}

esp_err_t MosaicoMatterCamera::Init(bridge_dev_mngr_handle_t dev_mngr)
{
    if (!dev_mngr || mDevMngr) return ESP_ERR_INVALID_ARG;
    mDevMngr = dev_mngr;
    snprintf(mProfile.id, sizeof(mProfile.id), "%s", kCameraProfileId);
    mProfile.device_type = kCameraDeviceType;
    mProfile.build_endpoint = BuildEndpoint;
    mProfile.user_ctx = this;
    bridge_dev_mngr_device_info_t devices[BRIDGE_DEV_MNGR_MAX_DEVICES]{};
    size_t count = 0;
    ESP_RETURN_ON_ERROR(bridge_dev_mngr_get_devices(mDevMngr, devices,
        BRIDGE_DEV_MNGR_MAX_DEVICES, &count), TAG, "Read camera catalog");
    for (size_t index = 0; index < count; ++index) {
        if (strcmp(devices[index].definition.profile_id, kCameraProfileId)) continue;
        if (mDevice.id[0]) return ESP_ERR_INVALID_STATE;
        mDevice = devices[index].definition;
        unsigned board = 0;
        unsigned long serial = 0;
        if (sscanf(mDevice.id, "mosaico-camera-%4x%8lx", &board, &serial) == 2) {
            mBoardId = static_cast<uint16_t>(board);
            mSerial = static_cast<uint32_t>(serial);
        }
    }
    return ESP_OK;
}

esp_err_t MosaicoMatterCamera::BuildEndpoint(void *endpoint,
                                             const char *device_id, void *ctx)
{
    (void)device_id;
    return static_cast<MosaicoMatterCamera *>(ctx)->ConfigureEndpoint(static_cast<endpoint_t *>(endpoint));
}

esp_err_t MosaicoMatterCamera::Register(bridge_dev_mngr_device_info_t &result)
{
    DevMngrWait *wait = NewDevMngrWait();
    if (!wait) return ESP_ERR_NO_MEM;
    bridge_dev_mngr_request_id_t request_id;
    esp_err_t error = bridge_dev_mngr_register_device(mDevMngr, &mDevice, &request_id,
                                               DevMngrCompleted, wait);
    return FinishDevMngrWait(wait, error, &result);
}

esp_err_t MosaicoMatterCamera::Activate(bridge_dev_mngr_device_info_t &result)
{
    DevMngrWait *wait = NewDevMngrWait();
    if (!wait) return ESP_ERR_NO_MEM;
    bridge_dev_mngr_request_id_t request_id;
    esp_err_t error = bridge_dev_mngr_activate_device(mDevMngr, mDevice.id, &mProfile,
                                               &request_id, DevMngrCompleted, wait);
    return FinishDevMngrWait(wait, error, &result);
}

esp_err_t MosaicoMatterCamera::SetReachable(
    bool reachable, bridge_dev_mngr_device_info_t *result)
{
    DevMngrWait *wait = NewDevMngrWait();
    if (!wait) return ESP_ERR_NO_MEM;
    bridge_dev_mngr_device_update_t update{};
    update.fields = BRIDGE_DEV_MNGR_UPDATE_REACHABLE;
    update.reachable = reachable;
    bridge_dev_mngr_request_id_t request_id;
    esp_err_t error = bridge_dev_mngr_update_device(mDevMngr, mDevice.id, &update,
                                             &request_id, DevMngrCompleted, wait);
    return FinishDevMngrWait(wait, error, result);
}

esp_err_t MosaicoMatterCamera::Deactivate()
{
    DevMngrWait *wait = NewDevMngrWait();
    if (!wait) return ESP_ERR_NO_MEM;
    bridge_dev_mngr_request_id_t request_id;
    esp_err_t error = bridge_dev_mngr_deactivate_device(mDevMngr, mDevice.id,
                                                 &request_id, DevMngrCompleted, wait);
    return FinishDevMngrWait(wait, error);
}

esp_err_t MosaicoMatterCamera::Remove()
{
    DevMngrWait *wait = NewDevMngrWait();
    if (!wait) return ESP_ERR_NO_MEM;
    bridge_dev_mngr_request_id_t request_id;
    esp_err_t error = bridge_dev_mngr_remove_device(mDevMngr, mDevice.id,
                                             &request_id, DevMngrCompleted, wait);
    return FinishDevMngrWait(wait, error);
}

esp_err_t MosaicoMatterCamera::ConfigureEndpoint(endpoint_t *endpoint)
{
    ESP_RETURN_ON_ERROR(endpoint::add_device_type(endpoint, kCameraDeviceType, 1), TAG, "Add camera device type");
    cluster::camera_av_stream_management::config_t av_cfg;
    av_cfg.max_content_buffer_size = kEncodedBufferSize;
    av_cfg.max_network_bandwidth = kMaxNetworkBandwidth;
    av_cfg.feature_flags = cluster::camera_av_stream_management::feature::video::get_id() |
        cluster::camera_av_stream_management::feature::audio::get_id() |
        cluster::camera_av_stream_management::feature::snapshot::get_id();
    auto *av_cluster = cluster::camera_av_stream_management::create(endpoint, &av_cfg, CLUSTER_FLAG_SERVER);
    if (!av_cluster) return ESP_ERR_NO_MEM;

    cluster::webrtc_transport_provider::config_t provider_cfg;
    if (!cluster::webrtc_transport_provider::create(endpoint, &provider_cfg, CLUSTER_FLAG_SERVER))
        return ESP_ERR_NO_MEM;
    cluster::webrtc_transport_requestor::config_t requestor_cfg;
    if (!cluster::webrtc_transport_requestor::create(endpoint, &requestor_cfg, CLUSTER_FLAG_CLIENT))
        return ESP_ERR_NO_MEM;

    uint16_t endpoint_id = endpoint::get_id(endpoint);
    CameraAvStreamManagementConfig integration;
    integration.delegate = this;
    integration.maxConcurrentEncoders = 1;
    integration.maxEncodedPixelRate = kWidth * kHeight * kFramesPerSecond;
    integration.videoSensorParams = { kWidth, kHeight, kFramesPerSecond, NullOptional };
    integration.minViewPortRes = { kWidth, kHeight };
    integration.rateDistortionTradeOffPoints.push_back(
        { VideoCodecEnum::kH264, { kWidth, kHeight }, 128000 });
    integration.maxContentBufferSize = kEncodedBufferSize;
    integration.microphoneCapabilities = { mosaico_camera_mock::kAudioChannels,
        Span<AudioCodecEnum>(sAudioCodecs), Span<uint32_t>(sAudioSampleRates), Span<uint8_t>(sAudioBitDepths) };
    integration.snapshotCapabilities.push_back(
        { { mosaico_camera_mock::kSnapshotWidth, mosaico_camera_mock::kSnapshotHeight },
          mosaico_camera_mock::kSnapshotFrameRate, ImageCodecEnum::kJpeg, false, NullOptional });
    integration.maxNetworkBandwidth = kMaxNetworkBandwidth;
    integration.supportedStreamUsages = { StreamUsageEnum::kLiveView };
    integration.streamUsagePriorities = { StreamUsageEnum::kLiveView };
    CameraAvStreamManagement::SetConfig(endpoint_id, integration);
    CameraAvStreamManagement::SetDelegate(endpoint_id, this);
    WebRTCTransportProvider::SetDelegate(endpoint_id, this);
    mEndpointId = endpoint_id;
    return ESP_OK;
}

esp_err_t MosaicoMatterCamera::Present(mosaico_module_mgr_slot_t slot, uint16_t board_id, uint32_t serial)
{
    if (mPresent) return ESP_ERR_INVALID_STATE;
    if (mCamera) {
        ESP_LOGW(TAG, "Retrying deferred camera cleanup before replug");
        if (!StopPeer()) {
            ESP_LOGE(TAG, "Camera tasks are still stopping; cannot reclaim the camera slot");
            return ESP_ERR_TIMEOUT;
        }
        ESP_RETURN_ON_ERROR(ReleaseCamera(), TAG, "Retry deferred camera cleanup");
    }
    bool same_module = mDevice.id[0] && mBoardId == board_id && mSerial == serial;
    if (mDevice.id[0] && !same_module) {
        ESP_RETURN_ON_ERROR(Remove(), TAG, "Remove replaced camera endpoint");
        mEndpointId = 0;
        mDevice = {};
    }
    if (!same_module) {
        mBoardId = board_id;
        mSerial = serial;
        FormatDevice(board_id, serial, mDevice);
    }

    ESP_RETURN_ON_ERROR(OpenCamera(slot), TAG, "Open camera capture");
    bridge_dev_mngr_device_info_t result{};
    esp_err_t err = Register(result);
    bool activated = false;
    if (err == ESP_OK) {
        err = Activate(result);
        activated = err == ESP_OK;
    }
    if (err == ESP_OK) err = SetReachable(true, &result);
    if (err != ESP_OK) {
        if (activated) {
            esp_err_t deactivate_err = Deactivate();
            if (deactivate_err != ESP_OK) {
                ESP_LOGE(TAG, "Rollback camera endpoint: %s",
                         esp_err_to_name(deactivate_err));
            }
        }
        esp_err_t cleanup_err = ReleaseCamera();
        if (cleanup_err != ESP_OK) {
            ESP_LOGE(TAG, "Camera endpoint setup failed and cleanup was deferred: %s",
                     esp_err_to_name(cleanup_err));
        }
        return err;
    }
    mDevice = result.definition;
    mEndpointId = result.endpoint_id;
    mSlot = slot;
    mPresent = true;
    ESP_LOGI(TAG, "Camera %04x-%08lx available on Matter endpoint %u", board_id,
             static_cast<unsigned long>(serial), mEndpointId);
    return ESP_OK;
}

esp_err_t MosaicoMatterCamera::OpenCamera(mosaico_module_mgr_slot_t slot)
{
    if (mCamera) return ESP_ERR_INVALID_STATE;

    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    config.slot = slot;
    config.pixel_format = V4L2_PIX_FMT_UYVY;
    config.buffer_count = 1;
    config.frame_timeout_ms = kCaptureTimeoutMs;
    const struct { uint16_t width; uint16_t height; } modes[] = {
        { kHdCaptureWidth, kHdCaptureHeight },
        { kVgaCaptureWidth, kVgaCaptureHeight },
    };
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    for (const auto &mode : modes) {
        config.width = mode.width;
        config.height = mode.height;
        err = mosaico_camera_new(&config, &mCamera);
        if (err == ESP_OK) {
            err = mosaico_camera_open(mCamera);
        }
        if (err == ESP_OK) {
            mosaico_camera_info_t info{};
            err = mosaico_camera_get_info(mCamera, &info);
            if (err == ESP_OK &&
                (info.width != mode.width || info.height != mode.height)) {
                ESP_LOGW(TAG, "Camera negotiated %lux%lu instead of %ux%u",
                         static_cast<unsigned long>(info.width),
                         static_cast<unsigned long>(info.height), mode.width,
                         mode.height);
                err = ESP_ERR_NOT_SUPPORTED;
            }
        }
        if (err == ESP_OK) {
            mCaptureWidth = mode.width;
            mCaptureHeight = mode.height;
            ESP_LOGI(TAG, "Camera capture ready at %ux%u with one buffer",
                     mode.width, mode.height);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Camera capture %ux%u unavailable: %s", mode.width,
                 mode.height, esp_err_to_name(err));
        if (mCamera) {
            ESP_RETURN_ON_ERROR(
                ReleaseCamera(), TAG,
                "Release camera after capture setup failure");
        }
    }
    return err;
}

esp_err_t MosaicoMatterCamera::ReleaseCamera()
{
    if (!mCamera) return ESP_OK;
    esp_err_t err = mosaico_camera_del(mCamera);
    if (err == ESP_OK) {
        mCamera = nullptr;
    } else {
        // mosaico_camera_del() deliberately leaves the handle and module lease
        // valid on failure so the caller can retry cleanup later.
        ESP_LOGW(TAG, "Camera cleanup deferred; retaining handle and module lease: %s",
                 esp_err_to_name(err));
    }
    return err;
}

void MosaicoMatterCamera::Absent(mosaico_module_mgr_slot_t slot)
{
    if (!mPresent || slot != mSlot) return;
    bool peer_stopped = StopPeer();
    esp_err_t err = Deactivate();
    if (err != ESP_OK) ESP_LOGE(TAG, "Disable camera endpoint: %s", esp_err_to_name(err));
    if (mCamera) {
        if (!peer_stopped) {
            ESP_LOGW(TAG, "Camera cleanup deferred until streaming tasks stop");
        } else {
            err = ReleaseCamera();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Camera will retry cleanup on the next plug event");
            }
        }
    }
    mCaptureWidth = 0;
    mCaptureHeight = 0;
    mPresent = false;
    mSlot = MOSAICO_MODULE_MGR_SLOT_AUTO;
    ESP_LOGI(TAG, "Camera unplugged; endpoint %u retained for replug", mEndpointId);
}

const std::vector<VideoStreamStruct> & MosaicoMatterCamera::GetAllocatedVideoStreams() const
{
    auto *server = CameraAvStreamManagement::GetServer(mEndpointId);
    return server ? server->GetAllocatedVideoStreams() : mEmptyVideo;
}

const std::vector<AudioStreamStruct> & MosaicoMatterCamera::GetAllocatedAudioStreams() const
{
    auto *server = CameraAvStreamManagement::GetServer(mEndpointId);
    return server ? server->GetAllocatedAudioStreams() : mEmptyAudio;
}

const std::vector<SnapshotStreamStruct> & MosaicoMatterCamera::GetAllocatedSnapshotStreams() const
{
    auto *server = CameraAvStreamManagement::GetServer(mEndpointId);
    return server ? server->GetAllocatedSnapshotStreams() : mEmptySnapshots;
}

CHIP_ERROR MosaicoMatterCamera::PersistentAttributesLoadedCallback()
{
    const auto &streams = GetAllocatedVideoStreams();
    mVideoAllocated = !streams.empty();
    mVideoConfig = streams.empty() ? VideoStreamStruct{} : streams.front();
    auto *server = CameraAvStreamManagement::GetServer(mEndpointId);
    VerifyOrReturnError(server, CHIP_ERROR_INCORRECT_STATE);
    ReturnErrorOnFailure(server->SetMicrophoneMinLevel(1));
    ReturnErrorOnFailure(server->SetMicrophoneMaxLevel(254));
    if (server->GetMicrophoneVolumeLevel() == 0) {
        ReturnErrorOnFailure(server->SetMicrophoneVolumeLevel(128));
    }
    return CHIP_NO_ERROR;
}

Status MosaicoMatterCamera::VideoStreamAllocate(const VideoStreamStruct &args, uint16_t &out_id)
{
    if (!mPresent || args.videoCodec != VideoCodecEnum::kH264 ||
        args.minResolution.width > kWidth || args.minResolution.height > kHeight ||
        args.maxResolution.width < kWidth || args.maxResolution.height < kHeight ||
        args.minFrameRate > kFramesPerSecond || args.maxFrameRate < kFramesPerSecond ||
        args.minBitRate > kMaxBitrate || args.streamUsage != StreamUsageEnum::kLiveView)
        return Status::DynamicConstraintError;
    const auto &streams = GetAllocatedVideoStreams();
    if (!streams.empty()) { out_id = streams.front().videoStreamID; return Status::Success; }
    out_id = kVideoStreamId;
    return Status::Success;
}

void MosaicoMatterCamera::OnVideoStreamAllocated(const VideoStreamStruct &stream, StreamAllocationAction)
{
    mVideoConfig = stream;
    mVideoAllocated = true;
}

Status MosaicoMatterCamera::VideoStreamModify(uint16_t stream_id, Optional<bool>, Optional<bool>)
{
    return ValidateVideoStreamID(stream_id) == CHIP_NO_ERROR ? Status::Success : Status::NotFound;
}

Status MosaicoMatterCamera::VideoStreamDeallocate(uint16_t stream_id)
{
    if (ValidateVideoStreamID(stream_id) != CHIP_NO_ERROR) return Status::NotFound;
    if (mSessionVideo && !StopPeer()) return Status::Failure;
    mVideoAllocated = false;
    return Status::Success;
}

Status MosaicoMatterCamera::AudioStreamAllocate(const AudioStreamStruct &args, uint16_t &out_id)
{
    using namespace mosaico_camera_mock;
    ESP_LOGI(TAG, "AudioStreamAllocate ep=%u present=%u usage=%u codec=%u channels=%u sample_rate=%lu bit_rate=%lu bit_depth=%u",
             mEndpointId, static_cast<unsigned>(mPresent.load()), static_cast<unsigned>(args.streamUsage),
             static_cast<unsigned>(args.audioCodec), args.channelCount, static_cast<unsigned long>(args.sampleRate),
             static_cast<unsigned long>(args.bitRate), args.bitDepth);
    if (!mPresent) {
        ESP_LOGW(TAG, "AudioStreamAllocate rejected: camera module is absent");
        return Status::InvalidInState;
    }
    if (args.streamUsage != StreamUsageEnum::kLiveView || args.audioCodec != AudioCodecEnum::kOpus ||
        !SupportsAudioFormat(args.channelCount, args.sampleRate, args.bitDepth, args.bitRate)) {
        ESP_LOGW(TAG, "AudioStreamAllocate rejected: require LiveView Opus mono 48000 Hz, 16/24/32-bit, 6000..128000 bit/s");
        return Status::DynamicConstraintError;
    }
    // AudioStreamAllocate cannot modify an existing allocation. Reuse only an
    // exact match so the reported configuration and transmitted packet agree.
    for (const auto &stream : GetAllocatedAudioStreams()) {
        if (stream.streamUsage == args.streamUsage && stream.audioCodec == args.audioCodec &&
            stream.channelCount == args.channelCount && stream.sampleRate == args.sampleRate &&
            stream.bitDepth == args.bitDepth && stream.bitRate == args.bitRate) {
            out_id = stream.audioStreamID;
            ESP_LOGI(TAG, "Reusing mock audio stream %u", out_id);
            return Status::Success;
        }
        ESP_LOGW(TAG, "AudioStreamAllocate rejected: deallocate stream %u before changing its format", stream.audioStreamID);
        return Status::ResourceExhausted;
    }
    out_id = kAudioStreamId;
    ESP_LOGI(TAG, "Allocated mock audio stream %u", out_id);
    return Status::Success;
}

Status MosaicoMatterCamera::AudioStreamDeallocate(uint16_t stream_id)
{
    if (ValidateAudioStreamID(stream_id) != CHIP_NO_ERROR) return Status::NotFound;
    if (mSessionAudio && !StopPeer()) return Status::Failure;
    return Status::Success;
}

Status MosaicoMatterCamera::SnapshotStreamAllocate(const SnapshotStreamAllocateArgs &args, uint16_t &out_id)
{
    using namespace mosaico_camera_mock;
    if (!mPresent || args.imageCodec != ImageCodecEnum::kJpeg || args.maxFrameRate > kSnapshotFrameRate ||
        args.minResolution.width > kSnapshotWidth || args.minResolution.height > kSnapshotHeight ||
        args.maxResolution.width < kSnapshotWidth || args.maxResolution.height < kSnapshotHeight) {
        return Status::DynamicConstraintError;
    }
    auto *server = CameraAvStreamManagement::GetServer(mEndpointId);
    if (!server) return Status::Failure;
    if (!GetAllocatedSnapshotStreams().empty()) {
        auto reusable = server->GetReusableSnapshotStreamId(args);
        if (!reusable.has_value()) return Status::ResourceExhausted;
        out_id = reusable.value();
    } else {
        out_id = kSnapshotStreamId;
    }
    return Status::Success;
}

Status MosaicoMatterCamera::SnapshotStreamDeallocate(uint16_t stream_id)
{
    for (const auto &stream : GetAllocatedSnapshotStreams()) {
        if (stream.snapshotStreamID == stream_id) return Status::Success;
    }
    return Status::NotFound;
}

Status MosaicoMatterCamera::CaptureSnapshot(DataModel::Nullable<uint16_t> stream_id,
    const VideoResolutionStruct &resolution, ImageSnapshot &out)
{
    using namespace mosaico_camera_mock;
    if (!mPresent) return Status::InvalidInState;
    const auto &streams = GetAllocatedSnapshotStreams();
    if (streams.empty()) return Status::NotFound;
    if (!stream_id.IsNull() && std::none_of(streams.begin(), streams.end(), [&](const auto &stream) {
            return stream.snapshotStreamID == stream_id.Value();
        })) return Status::NotFound;
    if (resolution.width != kSnapshotWidth || resolution.height != kSnapshotHeight)
        return Status::DynamicConstraintError;
    out.data.assign(std::begin(kSnapshotJpeg), std::end(kSnapshotJpeg));
    out.imageRes = { kSnapshotWidth, kSnapshotHeight };
    out.imageCodec = ImageCodecEnum::kJpeg;
    return Status::Success;
}

CHIP_ERROR MosaicoMatterCamera::ValidateVideoStreamID(uint16_t id)
{
    for (const auto &stream : GetAllocatedVideoStreams())
        if (stream.videoStreamID == id) return CHIP_NO_ERROR;
    return CHIP_ERROR_NOT_FOUND;
}

CHIP_ERROR MosaicoMatterCamera::ValidateVideoStreams(const std::vector<uint16_t> &ids)
{
    VerifyOrReturnError(ids.size() <= 1, CHIP_ERROR_INVALID_ARGUMENT);
    for (auto id : ids) ReturnErrorOnFailure(ValidateVideoStreamID(id));
    return CHIP_NO_ERROR;
}

CHIP_ERROR MosaicoMatterCamera::ValidateAudioStreamID(uint16_t id)
{
    for (const auto &stream : GetAllocatedAudioStreams())
        if (stream.audioStreamID == id) return CHIP_NO_ERROR;
    return CHIP_ERROR_NOT_FOUND;
}

CHIP_ERROR MosaicoMatterCamera::ValidateAudioStreams(const std::vector<uint16_t> &ids)
{
    VerifyOrReturnError(ids.size() <= 1, CHIP_ERROR_INVALID_ARGUMENT);
    for (auto id : ids) ReturnErrorOnFailure(ValidateAudioStreamID(id));
    return CHIP_NO_ERROR;
}

CHIP_ERROR MosaicoMatterCamera::ValidateStreamUsage(StreamUsageEnum usage,
    Optional<std::vector<uint16_t>> &video, Optional<std::vector<uint16_t>> &audio)
{
    VerifyOrReturnError(usage == StreamUsageEnum::kLiveView, CHIP_ERROR_NOT_FOUND);
    if (video.HasValue()) {
        if (video.Value().empty()) {
            const auto &streams = GetAllocatedVideoStreams();
            VerifyOrReturnError(!streams.empty(), CHIP_ERROR_NOT_FOUND);
            video.Value().push_back(streams.front().videoStreamID);
        }
        ReturnErrorOnFailure(ValidateVideoStreams(video.Value()));
    }
    if (audio.HasValue()) {
        if (audio.Value().empty()) {
            const auto &streams = GetAllocatedAudioStreams();
            VerifyOrReturnError(!streams.empty(), CHIP_ERROR_NOT_FOUND);
            audio.Value().push_back(streams.front().audioStreamID);
        }
        ReturnErrorOnFailure(ValidateAudioStreams(audio.Value()));
    }
    return video.HasValue() || audio.HasValue() ? CHIP_NO_ERROR : CHIP_ERROR_NOT_FOUND;
}

void MosaicoMatterCamera::FillSession(const OfferRequestArgs &args, WebRTCSessionStruct &out)
{
    out.id = args.sessionId;
    out.peerNodeID = args.peerNodeId;
    out.peerEndpointID = args.originatingEndpointId;
    out.streamUsage = args.streamUsage;
    out.fabricIndex = args.fabricIndex;
    // The endpoint has one fixed ID per media type. These Lists must outlive
    // CurrentSessions, so use the static IDs rather than the request vectors.
    if (args.videoStreams.HasValue() && !args.videoStreams.Value().empty()) {
        out.videoStreamID.SetNonNull(args.videoStreams.Value().front());
        out.videoStreams.SetValue(DataModel::List<const uint16_t>(&kVideoStreamId, 1));
    } else out.videoStreamID.SetNull();
    if (args.audioStreams.HasValue() && !args.audioStreams.Value().empty()) {
        out.audioStreamID.SetNonNull(args.audioStreams.Value().front());
        out.audioStreams.SetValue(DataModel::List<const uint16_t>(&kAudioStreamId, 1));
    } else out.audioStreamID.SetNull();
}

template <size_t N>
void copy_span(char (&out)[N], CharSpan span)
{
    size_t count = std::min(span.size(), N - 1);
    memcpy(out, span.data(), count);
    out[count] = 0;
}

void MosaicoMatterCamera::CopyIceServers(const Optional<std::vector<ICEServerDecodableStruct>> &servers)
{
    memset(mIceServers, 0, sizeof(mIceServers));
    mIceServerCount = 0;
    if (!servers.HasValue()) return;
    for (const auto &server : servers.Value()) {
        auto urls = server.URLs.begin();
        while (mIceServerCount < kMaxIceServers && urls.Next()) {
            size_t i = mIceServerCount++;
            copy_span(mIceUrls[i], urls.GetValue());
            if (server.username.HasValue()) copy_span(mIceUsers[i], server.username.Value());
            if (server.credential.HasValue()) copy_span(mIcePasswords[i], server.credential.Value());
            mIceServers[i].stun_url = mIceUrls[i];
            mIceServers[i].user = mIceUsers[i][0] ? mIceUsers[i] : nullptr;
            mIceServers[i].psw = mIcePasswords[i][0] ? mIcePasswords[i] : nullptr;
        }
    }
}

CHIP_ERROR MosaicoMatterCamera::StartPeer(const OfferRequestArgs &args, bool initiator,
                                          const std::string *remote_offer)
{
    VerifyOrReturnError(StopPeer(), CHIP_ERROR_TIMEOUT);
    VerifyOrReturnError(mPresent && mCamera, CHIP_ERROR_INCORRECT_STATE);
    mSessionVideo = args.videoStreams.HasValue() && !args.videoStreams.Value().empty();
    mSessionAudio = args.audioStreams.HasValue() && !args.audioStreams.Value().empty();
    VerifyOrReturnError(mSessionVideo || mSessionAudio, CHIP_ERROR_INCORRECT_STATE);
    if (mSessionAudio) {
        const auto &streams = GetAllocatedAudioStreams();
        auto stream = std::find_if(streams.begin(), streams.end(), [&](const auto &allocated) {
            return allocated.audioStreamID == args.audioStreams.Value().front();
        });
        VerifyOrReturnError(stream != streams.end(), CHIP_ERROR_NOT_FOUND);
        VerifyOrReturnError(mosaico_camera_mock::SupportsAudioFormat(stream->channelCount, stream->sampleRate,
                            stream->bitDepth, stream->bitRate), CHIP_ERROR_INVALID_ARGUMENT);
        mSessionAudioBitRate = stream->bitRate;
    }
    CopyIceServers(args.iceServers);
    mInitiator = initiator;
    mRemoteOffer = remote_offer ? *remote_offer : std::string{};
    std::string peer_offer;
    if (remote_offer && !mosaico_camera_sdp::NormalizeOffer(*remote_offer, peer_offer, mOfferedMedia)) {
        ESP_LOGE(TAG, "Unsupported WebRTC Offer media layout or MID values");
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
    if (remote_offer) {
        auto has_media = [&](const char *type) {
            return std::any_of(mOfferedMedia.begin(), mOfferedMedia.end(), [&](const auto &media) {
                return media.type == type;
            });
        };
        VerifyOrReturnError((!mSessionVideo || has_media("video")) && (!mSessionAudio || has_media("audio")),
                            CHIP_ERROR_INVALID_ARGUMENT);
    }
    mSessionId = args.sessionId;
    mPeerNode = ScopedNodeId(args.peerNodeId, args.fabricIndex);
    mPeerEndpoint = args.originatingEndpointId;
    esp_peer_default_cfg_t defaults{};
    defaults.rtp_cfg.send_pool_size = 256 * 1024;
    defaults.rtp_cfg.send_queue_num = 64;
    defaults.rtp_cfg.max_resend_count = 3;
    esp_peer_cfg_t cfg{};
    cfg.server_lists = mIceServerCount ? mIceServers : nullptr;
    cfg.server_num = mIceServerCount;
    cfg.role = initiator ? ESP_PEER_ROLE_CONTROLLING : ESP_PEER_ROLE_CONTROLLED;
    cfg.ice_trans_policy = args.iceTransportPolicy.HasValue() && args.iceTransportPolicy.Value() == "relay"
        ? ESP_PEER_ICE_TRANS_POLICY_RELAY : ESP_PEER_ICE_TRANS_POLICY_ALL;
    cfg.video_info = { ESP_PEER_VIDEO_CODEC_H264, kWidth, kHeight, kFramesPerSecond };
    cfg.video_dir = mSessionVideo ? ESP_PEER_MEDIA_DIR_SEND_ONLY : ESP_PEER_MEDIA_DIR_NONE;
    cfg.audio_info = { ESP_PEER_AUDIO_CODEC_OPUS, mosaico_camera_mock::kAudioSampleRate,
                      mosaico_camera_mock::kAudioChannels };
    cfg.audio_dir = mSessionAudio ? ESP_PEER_MEDIA_DIR_SEND_ONLY : ESP_PEER_MEDIA_DIR_NONE;
    cfg.no_auto_reconnect = true;
    cfg.extra_cfg = &defaults;
    cfg.extra_size = sizeof(defaults);
    cfg.ctx = this;
    cfg.on_state = PeerState;
    cfg.on_msg = PeerMessage;
    int peer_err = esp_peer_open(&cfg, esp_peer_get_default_impl(), &mPeer);
    VerifyOrReturnError(peer_err == ESP_PEER_ERR_NONE, CHIP_ERROR_INTERNAL);
    mPeerStopped = false;
    mPeerRun = true;
    if (xTaskCreateWithCaps(PeerTask, "matter_peer", 16 * 1024, this, 6, nullptr,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        mPeerRun = false; mPeerStopped = true; esp_peer_close(mPeer); mPeer = nullptr;
        return CHIP_ERROR_NO_MEMORY;
    }
    peer_err = esp_peer_new_connection(mPeer);
    if (peer_err == ESP_PEER_ERR_NONE && remote_offer) {
        CHIP_ERROR send_err = SendRemoteMessage(ESP_PEER_MSG_TYPE_SDP, peer_offer);
        if (send_err != CHIP_NO_ERROR) { StopPeer(); return send_err; }
    }
    if (peer_err != ESP_PEER_ERR_NONE) { StopPeer(); return CHIP_ERROR_INTERNAL; }
    return CHIP_NO_ERROR;
}

CHIP_ERROR MosaicoMatterCamera::HandleSolicitOffer(const OfferRequestArgs &args,
    WebRTCSessionStruct &out, bool &deferred)
{
    FillSession(args, out);
    deferred = false;
    return StartPeer(args, true, nullptr);
}

CHIP_ERROR MosaicoMatterCamera::HandleProvideOffer(const ProvideOfferRequestArgs &args,
    WebRTCSessionStruct &out)
{
    FillSession(args, out);
    return StartPeer(args, false, &args.sdp);
}

CHIP_ERROR MosaicoMatterCamera::SendRemoteMessage(esp_peer_msg_type_t type, const std::string &text)
{
    VerifyOrReturnError(mPeer, CHIP_ERROR_INCORRECT_STATE);
    esp_peer_msg_t message{ type, reinterpret_cast<uint8_t *>(const_cast<char *>(text.data())),
                            static_cast<int>(text.size()) };
    int peer_err = esp_peer_send_msg(mPeer, &message);
    if (peer_err != ESP_PEER_ERR_NONE)
        ESP_LOGE(TAG, "esp_peer rejected %s (%u bytes): %d",
                 type == ESP_PEER_MSG_TYPE_SDP ? "SDP" : "ICE candidate",
                 static_cast<unsigned>(text.size()), peer_err);
    return peer_err == ESP_PEER_ERR_NONE ? CHIP_NO_ERROR : CHIP_ERROR_INTERNAL;
}

CHIP_ERROR MosaicoMatterCamera::HandleProvideAnswer(uint16_t session, const std::string &answer)
{
    VerifyOrReturnError(mPeer && session == mSessionId, CHIP_ERROR_NOT_FOUND);
    VerifyOrReturnError(!answer.empty(), CHIP_ERROR_INVALID_ARGUMENT);
    if (answer.find("a=candidate:") != std::string::npos) {
        mPendingAnswer.clear();
        return SendRemoteMessage(ESP_PEER_MSG_TYPE_SDP, answer);
    }
    // The controller acknowledges Answer before sending its trickle candidates.
    // Keep the SDP until then so esp_peer can parse candidates in the SDP itself.
    mPendingAnswer = answer;
    ESP_LOGI(TAG, "Waiting for remote ICE candidates before applying answer SDP");
    return CHIP_NO_ERROR;
}

CHIP_ERROR MosaicoMatterCamera::HandleProvideICECandidates(uint16_t session,
    const std::vector<ICECandidateStruct> &candidates)
{
    VerifyOrReturnError(mPeer && session == mSessionId, CHIP_ERROR_NOT_FOUND);
    if (!mPendingAnswer.empty() || !mRemoteOffer.empty()) {
        std::string sdp;
        bool is_answer = !mPendingAnswer.empty();
        const std::string &base = is_answer ? mPendingAnswer : mRemoteOffer;
        ReturnErrorOnFailure(BuildSdpWithCandidates(base, candidates, sdp));
        ESP_LOGI(TAG, "Applying %s SDP with embedded remote ICE candidates",
                 is_answer ? "answer" : "updated offer");
        if (!is_answer) {
            std::string normalized;
            std::vector<mosaico_camera_sdp::OfferedMedia> media;
            VerifyOrReturnError(mosaico_camera_sdp::NormalizeOffer(sdp, normalized, media), CHIP_ERROR_INVALID_ARGUMENT);
            ReturnErrorOnFailure(SendRemoteMessage(ESP_PEER_MSG_TYPE_SDP, normalized));
        } else {
            ReturnErrorOnFailure(SendRemoteMessage(ESP_PEER_MSG_TYPE_SDP, sdp));
        }
        if (is_answer) mPendingAnswer.clear();
        else mRemoteOffer = std::move(sdp);
        return CHIP_NO_ERROR;
    }
    for (const auto &candidate : candidates) {
        std::string text(candidate.candidate.data(), candidate.candidate.size());
        ESP_LOGI(TAG, "Remote ICE candidate: %s", text.c_str());
        ReturnErrorOnFailure(SendRemoteMessage(ESP_PEER_MSG_TYPE_CANDIDATE, text));
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR MosaicoMatterCamera::BuildSdpWithCandidates(const std::string &base,
    const std::vector<ICECandidateStruct> &candidates, std::string &result)
{
    struct MediaSection {
        size_t begin;
        size_t end;
        std::string mid;
    };
    std::vector<MediaSection> sections;
    for (size_t begin = 0; begin < base.size();) {
        size_t end = base.find('\n', begin);
        size_t text_end = end == std::string::npos ? base.size() : end;
        if (text_end > begin && base[text_end - 1] == '\r') --text_end;
        if (base.compare(begin, 2, "m=") == 0) {
            if (!sections.empty()) sections.back().end = begin;
            sections.push_back({begin, base.size(), {}});
        } else if (!sections.empty() && base.compare(begin, 6, "a=mid:") == 0) {
            sections.back().mid = base.substr(begin + 6, text_end - begin - 6);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    VerifyOrReturnError(!sections.empty(), CHIP_ERROR_INVALID_ARGUMENT);

    std::vector<std::string> additions(sections.size());
    size_t added = 0;
    for (const auto &candidate : candidates) {
        std::string line(candidate.candidate.data(), candidate.candidate.size());
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.compare(0, 10, "candidate:") == 0) line.insert(0, "a=");
        if (line.compare(0, 12, "a=candidate:") != 0) {
            ESP_LOGW(TAG, "Ignoring malformed remote ICE candidate");
            continue;
        }

        size_t section = sections.size();
        if (!candidate.SDPMid.IsNull()) {
            auto mid_span = candidate.SDPMid.Value();
            std::string mid(mid_span.data(), mid_span.size());
            for (size_t i = 0; i < sections.size(); ++i)
                if (sections[i].mid == mid) { section = i; break; }
        }
        if (section == sections.size() && !candidate.SDPMLineIndex.IsNull() &&
            candidate.SDPMLineIndex.Value() < sections.size()) {
            section = candidate.SDPMLineIndex.Value();
        }
        if (section == sections.size() && sections.size() == 1) {
            section = 0;
        }
        if (section == sections.size()) {
            ESP_LOGW(TAG, "Ignoring ICE candidate with unknown SDP media section");
            continue;
        }
        ESP_LOGI(TAG, "Remote ICE candidate for mid %s: %s", sections[section].mid.c_str(), line.c_str());
        additions[section].append(line).append("\r\n");
        ++added;
    }
    VerifyOrReturnError(added > 0, CHIP_ERROR_INVALID_ARGUMENT);

    result = base.substr(0, sections.front().begin);
    for (size_t i = 0; i < sections.size(); ++i) {
        result.append(base, sections[i].begin, sections[i].end - sections[i].begin);
        if (!additions[i].empty()) {
            if (!result.empty() && result.back() != '\n') result.append("\r\n");
            result.append(additions[i]);
        }
    }
    ESP_LOGI(TAG, "Mapped %u ICE candidates into SDP", static_cast<unsigned>(added));
    return CHIP_NO_ERROR;
}

CHIP_ERROR MosaicoMatterCamera::HandleEndSession(uint16_t session, WebRTCEndReasonEnum)
{
    VerifyOrReturnError(session == mSessionId, CHIP_ERROR_NOT_FOUND);
    return StopPeer() ? CHIP_NO_ERROR : CHIP_ERROR_TIMEOUT;
}

int MosaicoMatterCamera::PeerState(esp_peer_state_t state, void *ctx)
{
    auto *self = static_cast<MosaicoMatterCamera *>(ctx);
    ESP_LOGI(TAG, "WebRTC peer state: %d", static_cast<int>(state));
    if (state == ESP_PEER_STATE_CONNECTED) {
        self->mPeerConnected = true;
        bool expected = false;
        if (self->mSessionVideo && self->mVideoRun.compare_exchange_strong(expected, true)) {
            self->mVideoStopped = false;
            if (xTaskCreateWithCaps(VideoTask, "matter_video", 12 * 1024, self, 5, nullptr,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
                self->mVideoRun = false;
                self->mVideoStopped = true;
                ESP_LOGE(TAG, "Unable to create H.264 video task");
            }
        }
    }
    if (state == ESP_PEER_STATE_DISCONNECTED || state == ESP_PEER_STATE_CONNECT_FAILED ||
        state == ESP_PEER_STATE_CLOSED) {
        self->mPeerConnected = false;
        self->mVideoRun = false;
    }
    return 0;
}

int MosaicoMatterCamera::PeerMessage(esp_peer_msg_t *message, void *ctx)
{
    auto *self = static_cast<MosaicoMatterCamera *>(ctx);
    if (!message || !message->data || message->size <= 0) return 0;
    if (message->type == ESP_PEER_MSG_TYPE_SDP)
    {
        if (self->mInitiator) {
            self->QueueOutbound(OutboundMessage::Type::Offer, self->mSessionId, message->data, message->size);
        } else if (!self->mAnswerQueued) {
            std::string peer_answer(reinterpret_cast<const char *>(message->data), message->size);
            while (!peer_answer.empty() && peer_answer.back() == '\0') peer_answer.pop_back();
            std::string answer;
            if (!mosaico_camera_sdp::BuildAnswer(peer_answer, self->mOfferedMedia,
                                                self->mSessionVideo, self->mSessionAudio, answer)) {
                ESP_LOGE(TAG, "esp_peer generated an invalid media Answer; not sending it to controller");
                return 0;
            }
            ESP_LOGI(TAG, "WebRTC media Answer: %u bytes (esp_peer generated %u bytes)",
                     static_cast<unsigned>(answer.size()), static_cast<unsigned>(peer_answer.size()));
            self->QueueOutbound(OutboundMessage::Type::Answer, self->mSessionId,
                                reinterpret_cast<const uint8_t *>(answer.data()), answer.size());
            self->mAnswerQueued = true;
        } else {
            // An updated remote offer may cause esp_peer to emit an answer again.
            // Matter has already delivered the initial answer to this session.
            ESP_LOGI(TAG, "Ignoring duplicate local answer after remote ICE update");
        }
    }
    else if (message->type == ESP_PEER_MSG_TYPE_CANDIDATE)
        self->QueueOutbound(OutboundMessage::Type::Candidate, self->mSessionId, message->data, message->size);
    return 0;
}

void MosaicoMatterCamera::PeerTask(void *ctx)
{
    auto *self = static_cast<MosaicoMatterCamera *>(ctx);
    std::array<uint8_t, mosaico_camera_mock::kMaxAudioPacketSize> silence;
    const size_t silence_size = mosaico_camera_mock::BuildOpusSilencePacket(self->mSessionAudioBitRate, silence);
    int64_t next_audio_ms = 0;
    bool audio_logged = false;
    while (self->mPeerRun) {
        esp_peer_main_loop(self->mPeer);
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (self->mSessionAudio && self->mPeerConnected && now_ms >= next_audio_ms) {
            esp_peer_audio_frame_t frame{ static_cast<uint32_t>(now_ms), silence.data(), static_cast<int>(silence_size) };
            int err = esp_peer_send_audio(self->mPeer, &frame);
            if (err == ESP_PEER_ERR_NONE && !audio_logged) {
                ESP_LOGI(TAG, "First mock Opus silence frame sent: %u bytes at %lu bit/s",
                         static_cast<unsigned>(silence_size), static_cast<unsigned long>(self->mSessionAudioBitRate));
                audio_logged = true;
            }
            // Do not burst old packets after a delayed peer loop.
            next_audio_ms = now_ms + mosaico_camera_mock::kAudioFrameDurationMs;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    self->mPeerStopped = true;
    vTaskDelete(nullptr);
}

bool MosaicoMatterCamera::StartVideo()
{
    if (!mCamera) {
        if (!mPresent || OpenCamera(mSlot) != ESP_OK) return false;
    }
    esp_err_t err = mosaico_camera_start_stream(mCamera);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Camera stream start failed (%s); rebuilding capture device "
                 "before one retry",
                 esp_err_to_name(err));
        if (ReleaseCamera() != ESP_OK || OpenCamera(mSlot) != ESP_OK) {
            return false;
        }
        err = mosaico_camera_start_stream(mCamera);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Camera stream retry failed: %s",
                     esp_err_to_name(err));
            return false;
        }
        ESP_LOGI(TAG, "Camera capture device recovered for streaming");
    }
    if (esp_video_enc_register_default() != ESP_VC_ERR_OK) {
        mosaico_camera_stop_stream(mCamera); return false;
    }
    mCodecRegistered = true;
    esp_video_enc_cfg_t cfg{};
    cfg.codec_type = ESP_VIDEO_CODEC_TYPE_H264;
    cfg.in_fmt = ESP_VIDEO_CODEC_PIXEL_FMT_YUV422;
    cfg.resolution = { kWidth, kHeight };
    cfg.fps = kFramesPerSecond;
    if (esp_video_enc_open(&cfg, &mEncoder) != ESP_VC_ERR_OK) {
        esp_video_enc_unregister_default(); mCodecRegistered = false;
        mosaico_camera_stop_stream(mCamera); return false;
    }
    uint32_t bitrate = mVideoAllocated ? std::min(mVideoConfig.maxBitRate, kMaxBitrate) : kDefaultBitrate;
    if (!bitrate) bitrate = kDefaultBitrate;
    esp_video_enc_set_bitrate(mEncoder, bitrate);
    esp_video_enc_set_gop(mEncoder, kFramesPerSecond * 2);
    mRawFrame = static_cast<uint8_t *>(heap_caps_malloc(kRawFrameSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    mEncodedFrame = static_cast<uint8_t *>(heap_caps_malloc(kEncodedBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!mRawFrame || !mEncodedFrame) { StopVideo(); return false; }
    return true;
}

void MosaicoMatterCamera::StopVideo()
{
    if (mEncoder) { esp_video_enc_close(mEncoder); mEncoder = nullptr; }
    free(mRawFrame); mRawFrame = nullptr;
    free(mEncodedFrame); mEncodedFrame = nullptr;
    if (mCamera) mosaico_camera_stop_stream(mCamera);
    if (mCodecRegistered) {
        esp_video_enc_unregister_default();
        mCodecRegistered = false;
    }
}

void MosaicoMatterCamera::VideoTask(void *ctx)
{
    auto *self = static_cast<MosaicoMatterCamera *>(ctx);
    if (!self->StartVideo()) {
        ESP_LOGE(TAG, "Unable to start H.264 video pipeline");
        self->mVideoRun = false;
    }
    const TickType_t frame_interval = pdMS_TO_TICKS(1000 / kFramesPerSecond);
    const TickType_t min_idle_time = pdMS_TO_TICKS(10);
    uint32_t encoded_frames = 0;
    uint32_t sent_frames = 0;
    bool first_frame_logged = false;
    int64_t last_stats_us = esp_timer_get_time();
    while (self->mVideoRun) {
        TickType_t frame_start = xTaskGetTickCount();
        mosaico_camera_frame_t camera_frame{};
        if (mosaico_camera_get_frame(self->mCamera, &camera_frame) != ESP_OK) {
            vTaskDelay(min_idle_time);
            continue;
        }
        if (camera_frame.width != self->mCaptureWidth || camera_frame.height != self->mCaptureHeight ||
            camera_frame.bytes_per_line < self->mCaptureWidth * 2 ||
            camera_frame.size < camera_frame.bytes_per_line * self->mCaptureHeight) {
            mosaico_camera_return_frame(self->mCamera, &camera_frame);
            ESP_LOGW(TAG, "Dropping unexpected camera frame layout");
            vTaskDelay(min_idle_time);
            continue;
        }
        const uint8_t *src = static_cast<const uint8_t *>(camera_frame.data);
        const size_t step = self->mCaptureHeight / kHeight;
        const size_t crop_left = (self->mCaptureWidth - kWidth * step) / 2;
        /* Center-crop HD to 4:3 when needed, then sample UYVY into YUYV. */
        for (size_t y = 0; y < kHeight; ++y) {
            const uint8_t *source_row = src + (y * step) * camera_frame.bytes_per_line + crop_left * 2;
            uint8_t *target_row = self->mRawFrame + y * kWidth * 2;
            for (size_t pair = 0; pair < kWidth / 2; ++pair) {
                const uint8_t *source = source_row + pair * 4 * step;
                uint8_t *target = target_row + pair * 4;
                target[0] = source[1];
                target[1] = (static_cast<unsigned>(source[0]) + source[4]) / 2;
                target[2] = source[step == 3 ? 7 : 5];
                target[3] = (static_cast<unsigned>(source[2]) + source[6]) / 2;
            }
        }
        mosaico_camera_return_frame(self->mCamera, &camera_frame);
        esp_video_enc_in_frame_t input{};
        input.pts = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        input.data = self->mRawFrame;
        input.size = kRawFrameSize;
        esp_video_enc_out_frame_t output{};
        output.data = self->mEncodedFrame;
        output.size = kEncodedBufferSize;
        int64_t encode_start_us = esp_timer_get_time();
        esp_vc_err_t encode_err = esp_video_enc_process(self->mEncoder, &input, &output);
        int64_t encode_ms = (esp_timer_get_time() - encode_start_us) / 1000;
        if (encode_ms > 1000) {
            ESP_LOGW(TAG, "Software H.264 encode took %lld ms; requested %u fps may be unsustainable",
                     static_cast<long long>(encode_ms), kFramesPerSecond);
        }
        if (encode_err == ESP_VC_ERR_OK && output.encoded_size) {
            ++encoded_frames;
            esp_peer_video_frame_t peer_frame{ output.pts, output.data, static_cast<int>(output.encoded_size) };
            int send_err = esp_peer_send_video(self->mPeer, &peer_frame);
            if (send_err == ESP_PEER_ERR_NONE) {
                ++sent_frames;
                if (!first_frame_logged) {
                    ESP_LOGI(TAG, "First H.264 frame sent: %u bytes, encode=%lld ms",
                             static_cast<unsigned>(output.encoded_size), static_cast<long long>(encode_ms));
                    first_frame_logged = true;
                }
            } else {
                ESP_LOGW(TAG, "esp_peer_send_video failed: %d", send_err);
            }
        } else if (encode_err != ESP_VC_ERR_OK) {
            ESP_LOGW(TAG, "H.264 encode failed: %d", encode_err);
        }
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_stats_us >= 5000000) {
            ESP_LOGI(TAG, "Video pipeline in 5 s: encoded=%lu sent=%lu",
                     static_cast<unsigned long>(encoded_frames), static_cast<unsigned long>(sent_frames));
            encoded_frames = 0;
            sent_frames = 0;
            last_stats_us = now_us;
        }
        // vTaskDelayUntil does not block after a missed deadline. A slow software
        // encoder would then starve the CPU's idle task and trip its watchdog.
        TickType_t elapsed = xTaskGetTickCount() - frame_start;
        vTaskDelay(elapsed >= frame_interval ? min_idle_time
                                             : std::max(min_idle_time, frame_interval - elapsed));
    }
    self->StopVideo();
    self->mVideoStopped = true;
    vTaskDelete(nullptr);
}

bool MosaicoMatterCamera::StopPeer()
{
    mPeerConnected = false;
    mVideoRun = false;
    mPeerRun = false;
    for (int i = 0; i < 100 && !mVideoStopped; ++i) vTaskDelay(pdMS_TO_TICKS(20));
    for (int i = 0; i < 100 && !mPeerStopped; ++i) vTaskDelay(pdMS_TO_TICKS(20));
    if (!mVideoStopped || !mPeerStopped) {
        ESP_LOGE(TAG, "Timed out stopping camera tasks (video=%s peer=%s)",
                 mVideoStopped ? "stopped" : "running",
                 mPeerStopped ? "stopped" : "running");
        return false;
    }
    if (mPeer) { esp_peer_close(mPeer); mPeer = nullptr; }
    mSessionId = 0;
    mSessionVideo = false;
    mSessionAudio = false;
    mPendingAnswer.clear();
    mRemoteOffer.clear();
    mOfferedMedia.clear();
    mAnswerQueued = false;
    mOutboundRetries = 0;
    return true;
}

void MosaicoMatterCamera::QueueOutbound(OutboundMessage::Type type, uint16_t session,
                                        const uint8_t *data, size_t size)
{
    auto *message = new (std::nothrow) OutboundMessage{ type, session,
        std::string(reinterpret_cast<const char *>(data), size) };
    if (!message) return;
    while (!message->payload.empty() && message->payload.back() == '\0') message->payload.pop_back();
    if (DeviceLayer::PlatformMgr().ScheduleWork(DeliverOutbound, reinterpret_cast<intptr_t>(message)) != CHIP_NO_ERROR)
        delete message;
}

void MosaicoMatterCamera::DeliverOutbound(intptr_t arg)
{
    auto *message = reinterpret_cast<OutboundMessage *>(arg);
    if (sCamera.mPresent && message->session_id == sCamera.mSessionId) {
        sCamera.mOutbound.push_back(std::move(*message));
        sCamera.PumpOutbound();
    }
    delete message;
}

void MosaicoMatterCamera::PumpOutbound()
{
    if (mOutboundBusy) return;
    while (!mOutbound.empty() && mOutbound.front().session_id != mSessionId)
        mOutbound.pop_front();
    if (mOutbound.empty() || !mPeerNode.IsOperational()) return;
    mOutboundBusy = true;
    auto *manager = Server::GetInstance().GetCASESessionManager();
    if (!manager) { OutboundDone(false); return; }
    bool large = mOutbound.front().payload.size() > kMaxSmallSignalBytes;
    manager->FindOrEstablishSession(mPeerNode, &mOnConnected, &mOnConnectionFailure,
                                    large ? TransportPayloadCapability::kLargePayload
                                          : TransportPayloadCapability::kMRPPayload);
}

CHIP_ERROR MosaicoMatterCamera::SendFront(Messaging::ExchangeManager &exchange,
                                           const SessionHandle &session)
{
    VerifyOrReturnError(!mOutbound.empty(), CHIP_ERROR_INCORRECT_STATE);
    const auto &front = mOutbound.front();
    bool large = front.payload.size() > kMaxSmallSignalBytes;
    ESP_LOGI(TAG, "Sending WebRTC signal type=%u session=%u payload=%u bytes over %s-compatible CASE",
             static_cast<unsigned>(front.type), front.session_id,
             static_cast<unsigned>(front.payload.size()), large ? "TCP" : "MRP");
    auto success = [this](const ConcreteCommandPath &, const StatusIB &, const auto &) { OutboundDone(true); };
    auto failure = [this](CHIP_ERROR error) { ESP_LOGE(TAG, "WebRTC Matter signal failed: %s", ErrorStr(error)); OutboundDone(false); };
    if (front.type == OutboundMessage::Type::Offer) {
        WebRTCTransportRequestor::Commands::Offer::Type command;
        command.webRTCSessionID = front.session_id;
        command.sdp = CharSpan::fromCharString(front.payload.c_str());
        return Controller::InvokeCommandRequest(&exchange, session, mPeerEndpoint, command, success, failure,
                                                NullOptional, NullOptional, nullptr, large);
    }
    if (front.type == OutboundMessage::Type::Answer) {
        WebRTCTransportRequestor::Commands::Answer::Type command;
        command.webRTCSessionID = front.session_id;
        command.sdp = CharSpan::fromCharString(front.payload.c_str());
        return Controller::InvokeCommandRequest(&exchange, session, mPeerEndpoint, command, success, failure,
                                                NullOptional, NullOptional, nullptr, large);
    }
    ICECandidateStruct candidate{ CharSpan::fromCharString(front.payload.c_str()) };
    WebRTCTransportRequestor::Commands::ICECandidates::Type command;
    command.webRTCSessionID = front.session_id;
    command.ICECandidates = DataModel::List<const ICECandidateStruct>(&candidate, 1);
    return Controller::InvokeCommandRequest(&exchange, session, mPeerEndpoint, command, success, failure,
                                            NullOptional, NullOptional, nullptr, large);
}

void MosaicoMatterCamera::OutboundDone(bool success)
{
    if (!mOutbound.empty()) mOutbound.pop_front();
    mOutboundBusy = false;
    mOutboundRetries = 0;
    if (!success) ESP_LOGW(TAG, "Dropping failed WebRTC signaling message");
    PumpOutbound();
}

void MosaicoMatterCamera::OnDeviceConnected(void *ctx, Messaging::ExchangeManager &exchange,
                                             const SessionHandle &session)
{
    auto *self = static_cast<MosaicoMatterCamera *>(ctx);
    CHIP_ERROR err = self->SendFront(exchange, session);
    if (err == CHIP_NO_ERROR) return;
    ESP_LOGE(TAG, "Could not start WebRTC Matter signal: %s", ErrorStr(err));
    if (err == CHIP_ERROR_INVALID_ARGUMENT && self->mOutboundRetries++ == 0 &&
        !self->mOutbound.empty() && self->mOutbound.front().payload.size() <= kMaxSmallSignalBytes) {
        ESP_LOGW(TAG, "Retrying small WebRTC signal after CASE session became unusable");
        self->mOutboundBusy = false;
        self->PumpOutbound();
        return;
    }
    self->OutboundDone(false);
}

void MosaicoMatterCamera::OnDeviceConnectionFailure(void *ctx, const ScopedNodeId &, CHIP_ERROR error)
{
    ESP_LOGE(TAG, "CASE session for WebRTC signaling failed: %s", ErrorStr(error));
    static_cast<MosaicoMatterCamera *>(ctx)->OutboundDone(false);
}

} // namespace

extern "C" esp_err_t mosaico_matter_camera_init(bridge_dev_mngr_handle_t dev_mngr)
{
    return sCamera.Init(dev_mngr);
}

extern "C" esp_err_t mosaico_matter_camera_present(mosaico_module_mgr_slot_t slot,
                                                     uint16_t board_id, uint32_t serial_number)
{
    return sCamera.Present(slot, board_id, serial_number);
}

extern "C" void mosaico_matter_camera_absent(mosaico_module_mgr_slot_t slot)
{
    sCamera.Absent(slot);
}
