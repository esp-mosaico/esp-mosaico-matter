# Adapt build-directory copies only; never mutate managed or upstream sources.
function(camera_pool_replace BEFORE AFTER EXPECTED)
    string(REPLACE "${BEFORE}" "" _without "${_camera_content}")
    string(LENGTH "${_camera_content}" _original_size)
    string(LENGTH "${_without}" _remaining_size)
    string(LENGTH "${BEFORE}" _pattern_size)
    math(EXPR _matches "(${_original_size} - ${_remaining_size}) / ${_pattern_size}")
    if(NOT _matches EQUAL EXPECTED)
        message(FATAL_ERROR "Camera pool adapter: expected ${EXPECTED} matches, found ${_matches} in ${_camera_source}: ${BEFORE}")
    endif()
    string(REPLACE "${BEFORE}" "${AFTER}" _updated "${_camera_content}")
    set(_camera_content "${_updated}" PARENT_SCOPE)
endfunction()

macro(camera_pool_open COMPONENT RELATIVE)
    idf_component_get_property(_camera_lib ${COMPONENT} COMPONENT_LIB)
    idf_component_get_property(_camera_dir ${COMPONENT} COMPONENT_DIR)
    set(_camera_source "${_camera_dir}/${RELATIVE}")
    file(READ "${_camera_source}" _camera_content)
endmacro()

function(camera_pool_finish NAME)
    set(_copy "${CMAKE_BINARY_DIR}/camera_pool_adapters/${NAME}")
    file(CONFIGURE OUTPUT "${_copy}" CONTENT "${_camera_content}" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_camera_source}")
    get_target_property(_sources ${_camera_lib} SOURCES)
    list(FIND _sources "${_camera_source}" _source_index)
    if(_source_index EQUAL -1)
        message(FATAL_ERROR "Camera pool adapter source missing from ${_camera_lib}: ${_camera_source}")
    endif()
    list(REMOVE_ITEM _sources "${_camera_source}")
    set_property(TARGET ${_camera_lib} PROPERTY SOURCES "${_sources}")
    target_sources(${_camera_lib} PRIVATE "${_copy}")
    target_link_libraries(${_camera_lib} PRIVATE ${_camera_pool_lib})
endfunction()

idf_component_get_property(_camera_pool_lib camera_buffer_pool COMPONENT_LIB)
idf_build_get_property(_camera_components BUILD_COMPONENTS)
set(_camera_video_component "")
foreach(_candidate tda-2030__claw_video espressif__esp_video esp_video)
    if(_candidate IN_LIST _camera_components)
        idf_component_get_property(_candidate_dir ${_candidate} COMPONENT_DIR)
        if(EXISTS "${_candidate_dir}/src/device/esp_video_device_common.c")
            set(_camera_video_component ${_candidate})
            break()
        endif()
    endif()
endforeach()
if(NOT _camera_video_component)
    message(FATAL_ERROR "Camera buffer pool requires the esp_video device implementation")
endif()

camera_pool_open(mosaico_module_camera "mosaico_camera.c")
camera_pool_replace([====[#include "mosaico_module_camera.h"]====] [====[#include "mosaico_module_camera.h"
#include "camera_buffer_pool.h"]====] 1)
camera_pool_replace([====[static uint32_t count_outstanding_buffers(mosaico_camera_handle_t camera)]====] [====[static esp_err_t camera_queue_buffer(mosaico_camera_handle_t camera,
                                      struct v4l2_buffer *buffer)
{
    if (buffer->index >= camera->info.buffer_count ||
        !camera->buffers[buffer->index]) {
        return ESP_ERR_INVALID_ARG;
    }
    buffer->memory = V4L2_MEMORY_USERPTR;
    buffer->m.userptr = (unsigned long)(uintptr_t)camera->buffers[buffer->index];
    buffer->length = camera->buffer_lengths[buffer->index];
    return camera_ioctl(camera->fd, VIDIOC_QBUF, buffer, "VIDIOC_QBUF");
}

static uint32_t count_outstanding_buffers(mosaico_camera_handle_t camera)]====] 1)
camera_pool_replace([====[.memory = V4L2_MEMORY_MMAP,]====] [====[.memory = V4L2_MEMORY_USERPTR,]====] 7)
camera_pool_replace([====[camera_ioctl(camera->fd, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF")]====] [====[camera_queue_buffer(camera, &buffer)]====] 6)
camera_pool_replace([====[        camera->buffers[i] =
            mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                 camera->fd, buffer.m.offset);
        ESP_RETURN_ON_FALSE(camera->buffers[i] != MAP_FAILED,
                            ESP_ERR_NO_MEM, TAG,
                            "mmap camera buffer %" PRIu32 " failed errno=%d",
                            i, errno);
        camera->buffer_lengths[i] = buffer.length;]====] [====[        ESP_RETURN_ON_ERROR(camera_buffer_pool_capture_get(
            camera, i, &camera->buffers[i], &camera->buffer_lengths[i]),
            TAG, "get reserved capture buffer failed");
        ESP_RETURN_ON_FALSE(camera->buffer_lengths[i] >= buffer.length,
                            ESP_ERR_INVALID_SIZE, TAG, "capture pool frame too small");]====] 1)
camera_pool_replace([====[    camera->info.buffer_count = (uint8_t)request.count;]====] [====[    camera->info.buffer_count = (uint8_t)request.count;
    ESP_RETURN_ON_ERROR(camera_buffer_pool_capture_acquire(
        camera, camera->info.frame_buffer_size, request.count),
        TAG, "capture pool unavailable");
    ESP_LOGI(TAG, "Using reserved capture buffers: count=%u bytes=%u",
             (unsigned)request.count, (unsigned)camera->info.frame_buffer_size);]====] 1)
camera_pool_replace([====[            if (munmap(camera->buffers[i], camera->buffer_lengths[i]) != 0) {
                ESP_LOGE(TAG, "munmap buffer %u failed: errno=%d", (unsigned)i, errno);
                return ESP_FAIL;
            }
]====] [====[]====] 1)
camera_pool_replace([====[    const mosaico_module_mgr_slot_t slot = camera->info.slot;]====] [====[    ESP_RETURN_ON_ERROR(camera_buffer_pool_capture_release(camera), TAG,
                        "release capture pool lease failed");
    const mosaico_module_mgr_slot_t slot = camera->info.slot;]====] 1)
include("${CMAKE_CURRENT_LIST_DIR}/recovery_camera.cmake")
camera_pool_finish("mosaico_camera.c")

camera_pool_open(${_camera_video_component} "src/device/esp_video_dvp_device.c")
camera_pool_replace([====[#include "esp_video.h"]====] [====[#include "esp_video.h"
#include "camera_buffer_pool.h"]====] 1)
camera_pool_replace([====[        .pin_dont_init = true,]====] [====[        .pin_dont_init = true,
        .bk_buffer_dis = true,]====] 1)
camera_pool_replace([====[    ret = esp_cam_new_dvp_ctlr_ext(&dvp_config, &cam_ctrl_handle);
    if (ret != ESP_OK) {]====] [====[    struct esp_video_buffer_info buffer_info;
    ret = esp_video_get_buffer_info(common->video, V4L2_BUF_TYPE_VIDEO_CAPTURE, &buffer_info);
    if (ret == ESP_OK) {
        ret = camera_buffer_pool_discard_acquire(common, buffer_info.size);
    }
    bool discard_acquired = ret == ESP_OK;
    if (ret == ESP_OK) {
        ret = esp_cam_new_dvp_ctlr_ext(&dvp_config, &cam_ctrl_handle);
    }
    if (ret == ESP_OK) {
        ret = camera_buffer_pool_discard_bind(common, cam_ctrl_handle);
        if (ret != ESP_OK) {
            esp_cam_ctlr_del(cam_ctrl_handle);
        }
    }
    if (ret != ESP_OK) {
        if (discard_acquired) {
            camera_buffer_pool_discard_release(common);
        }]====] 1)
camera_pool_finish("esp_video_dvp_device.c")

camera_pool_open(${_camera_video_component} "src/device/esp_video_device_common.c")
camera_pool_replace([====[#include "esp_video.h"]====] [====[#include "esp_video.h"
#include "camera_buffer_pool.h"]====] 1)
camera_pool_replace([====[    bool need_yield = false;
    struct esp_video *video = (struct esp_video *)user_data;]====] [====[    if (camera_buffer_pool_is_discard(handle, trans->buffer)) {
        return false;
    }
    bool need_yield = false;
    struct esp_video *video = (struct esp_video *)user_data;]====] 1)
camera_pool_replace([====[    element = CAPTURE_VIDEO_GET_QUEUED_ELEMENT(video);
    if (!element) {]====] [====[    element = CAPTURE_VIDEO_GET_QUEUED_ELEMENT(video);
    if (!element) {
        size_t bytes = 0;
        void *discard = camera_buffer_pool_discard_buffer(handle, &bytes);
        if (discard) {
            trans->buffer = discard;
            trans->buflen = bytes;
            return false;
        }]====] 1)
camera_pool_replace([====[    esp_cam_ctlr_del(common->cam_ctrl_handle);
    common->cam_ctrl_handle = NULL;]====] [====[    esp_err_t delete_error = esp_cam_ctlr_del(common->cam_ctrl_handle);
    if (delete_error != ESP_OK) {
        return delete_error;
    }
    camera_buffer_pool_discard_release(common);
    common->cam_ctrl_handle = NULL;]====] 1)
camera_pool_replace([====[    ESP_RETURN_ON_ERROR(esp_cam_ctlr_del(common->cam_ctrl_handle), TAG, "failed to delete CAM ctlr");
    common->cam_ctrl_handle = NULL;]====] [====[    ESP_RETURN_ON_ERROR(esp_cam_ctlr_del(common->cam_ctrl_handle), TAG, "failed to delete CAM ctlr");
    camera_buffer_pool_discard_release(common);
    common->cam_ctrl_handle = NULL;]====] 1)
camera_pool_replace([====[        ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(common->cam.sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &flags),
                            TAG, "failed to stop sensor stream");]====] [====[        esp_err_t sensor_error = esp_cam_sensor_ioctl(common->cam.sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &flags);
        if (sensor_error != ESP_OK) {
            ESP_LOGW(TAG, "Sensor stop failed (%s); continuing controller teardown",
                     esp_err_to_name(sensor_error));
        }]====] 1)
camera_pool_finish("esp_video_device_common.c")

camera_pool_open(mosaico_camera_service "mosaico_camera_service.c")
include("${CMAKE_CURRENT_LIST_DIR}/recovery_service.cmake")
camera_pool_finish("mosaico_camera_service.c")
