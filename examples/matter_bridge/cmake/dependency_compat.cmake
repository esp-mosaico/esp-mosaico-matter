# SPDX-License-Identifier: Apache-2.0
# Compatibility fixes for the dependencies used by this example.
# Included after project(), when component targets and generated headers exist.

# Use PSA multipart AEAD for large TCP signaling, as in connectedhomeip #74685.
# ESP-Matter generates this header during project(); update it on each configure
# so a freshly downloaded dependency needs no source patch.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    idf_component_get_property(BRIDGE_MATTER_LIB espressif__esp_matter COMPONENT_LIB)
    get_target_property(BRIDGE_MATTER_BUILD_DIR ${BRIDGE_MATTER_LIB} BINARY_DIR)
    set(BRIDGE_MATTER_CRYPTO_CONFIG "${BRIDGE_MATTER_BUILD_DIR}/crypto/CryptoBuildConfig.h")
    file(READ "${BRIDGE_MATTER_CRYPTO_CONFIG}" BRIDGE_MATTER_CRYPTO_CONTENT)
    string(REPLACE
        "#define CHIP_CRYPTO_PSA_AEAD_SINGLE_PART 1"
        "#define CHIP_CRYPTO_PSA_AEAD_SINGLE_PART 0"
        BRIDGE_MATTER_CRYPTO_UPDATED "${BRIDGE_MATTER_CRYPTO_CONTENT}")
    if(NOT BRIDGE_MATTER_CRYPTO_UPDATED MATCHES "#define CHIP_CRYPTO_PSA_AEAD_SINGLE_PART 0")
        message(FATAL_ERROR "Unable to enable PSA multipart AEAD in ${BRIDGE_MATTER_CRYPTO_CONFIG}")
    endif()
    if(NOT BRIDGE_MATTER_CRYPTO_CONTENT STREQUAL BRIDGE_MATTER_CRYPTO_UPDATED)
        file(WRITE "${BRIDGE_MATTER_CRYPTO_CONFIG}" "${BRIDGE_MATTER_CRYPTO_UPDATED}")
    endif()
endif()

# app_claw's native board bridge includes Board Manager even when its raw Lua
# module is disabled. Supply the dependency without editing the upstream tree.
idf_component_get_property(BRIDGE_CLAW_LIB app_claw COMPONENT_LIB)
idf_component_get_property(BRIDGE_BOARD_LIB espressif__esp_board_manager COMPONENT_LIB)
target_link_libraries(${BRIDGE_CLAW_LIB} PRIVATE ${BRIDGE_BOARD_LIB})

# GCC 16 treats non-C typedef for linkage in third-party esp-dl headers as an error.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
    idf_component_get_property(BRIDGE_ESP_DL_LIB espressif__esp-dl COMPONENT_LIB)
    target_compile_options(${BRIDGE_ESP_DL_LIB} PRIVATE -Wno-error=non-c-typedef-for-linkage)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/apply_dependency_patch.cmake")
idf_component_get_property(camera_sensor_dir espressif__esp_cam_sensor COMPONENT_DIR)
idf_component_get_property(claw_video_dir tda-2030__claw_video COMPONENT_DIR)
bridge_apply_dependency_patch("${camera_sensor_dir}"
    "${CMAKE_CURRENT_LIST_DIR}/../patches/sc101iot-pclk-delay.patch")
bridge_apply_dependency_patch("${claw_video_dir}"
    "${CMAKE_CURRENT_LIST_DIR}/../patches/claw-video-stop-cleanup.patch")
