# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "mosaico_matter_camera.cpp" "sdp_compat.cpp"
    INCLUDE_DIRS "include"
    REQUIRES matter_bridge mosaico_module_camera esp_peer esp_video_codec
    PRIV_REQUIRES esp_matter freertos heap
)
