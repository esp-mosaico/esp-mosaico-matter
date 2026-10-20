cmake_minimum_required(VERSION 3.16)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(UPSTREAM "${REPO_ROOT}/third_party/esp-mosaico-claw")
# Select components explicitly: the repo's http_server is a controller fork.
# Keep the controller-only stack out of this example. mosaico_ui's CMake
# optional_requires the matching backend, but its YAML must not path-depend
# on local_fabric/claw_matter_controller (that would pull them into this
# project). EXCLUDE_COMPONENTS is belt-and-braces if another dep lists them.
list(APPEND EXCLUDE_COMPONENTS local_fabric claw_matter_controller thread_border_router)
list(APPEND EXTRA_COMPONENT_DIRS
    "${REPO_ROOT}/components/matter_bridge"
    "${REPO_ROOT}/components/claw_matter_bridge"
    "${REPO_ROOT}/components/mosaico_ui"
    "${UPSTREAM}/components/app_settings_service"
    "${UPSTREAM}/components/app_system_config"
    "${UPSTREAM}/components/display_service"
    "${UPSTREAM}/components/esp_lcd_touch_mux"
    "${UPSTREAM}/components/wifi_manager"
    "${UPSTREAM}/third-party/esp-claw/components/common/settings")

# Extra component directories outrank managed dependencies, so this esp_video
# shim overrides the registry copy used by the camera components.
list(APPEND EXTRA_COMPONENT_DIRS
    "${UPSTREAM}/third-party/esp-claw/components/common/esp_video")

# Shared UI components read these CACHE variables in a separate registration
# scope. The bridge uses its own app roster and Matter server backend.
set(MOSAICO_UI_APP_MANIFESTS hub welcome ai_create settings setup_center matter_bridge
    CACHE STRING "" FORCE)
set(MOSAICO_UI_WITH_MATTER_BRIDGE ON CACHE BOOL "" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ESP-IDF 6.2 dropped esp_timer from the common requires that every component
# gets implicitly. Several upstream esp-mosaico-claw components (the
# esp_mosaico board's usb_hs_console, trial_provider, ...) include esp_timer.h
# without declaring it, so restore it build-wide rather than patching each
# upstream CMakeLists.
idf_build_set_property(__COMPONENT_REQUIRES_COMMON esp_timer APPEND)

project(matter_bridge VERSION 0.1.0)

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

if(NOT CONFIG_PARTITION_TABLE_CUSTOM OR NOT CONFIG_SPIRAM OR
   NOT CONFIG_ESPTOOLPY_FLASHSIZE_16MB)
    # Leave configuration usable so menuconfig can repair an existing sdkconfig.
    message(WARNING
        "matter_bridge requires a custom partition table, 16 MB flash and PSRAM. "
        "An existing sdkconfig may have retained ESP-IDF defaults. Run idf.py menuconfig: "
        "select partitions_16MB.csv with offset 0x9000, 16 MB flash, "
        "and octal PSRAM at 250 MHz.")
endif()

# app_claw's native board bridge includes Board Manager even when its raw Lua
# module is disabled. Supply the dependency without editing the upstream tree.
idf_component_get_property(BRIDGE_CLAW_LIB app_claw COMPONENT_LIB)
idf_component_get_property(BRIDGE_BOARD_LIB espressif__esp_board_manager COMPONENT_LIB)
target_link_libraries(${BRIDGE_CLAW_LIB} PRIVATE ${BRIDGE_BOARD_LIB})
set(SYSTEM_IMAGE "${CMAKE_BINARY_DIR}/system_fs_image")
add_custom_target(bridge_prepare_system ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SYSTEM_IMAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${UPSTREAM}/fatfs_image/system" "${SYSTEM_IMAGE}"
    VERBATIM)
littlefs_create_partition_image(system "${SYSTEM_IMAGE}" FLASH_IN_PROJECT)
skill_builder_configure_skill_sync(TARGET littlefs_system_bin SKILL_OUTPUT_DIR "${SYSTEM_IMAGE}/skills")
lua_module_builder_configure_builtin_lua_sync(TARGET littlefs_system_bin
    BUILTIN_OUTPUT_DIR "${SYSTEM_IMAGE}/scripts/builtin"
    DOCS_OUTPUT_DIR "${SYSTEM_IMAGE}/scripts/docs")
add_dependencies(skill_builder_sync_skills bridge_prepare_system)
add_dependencies(lua_module_builder_sync_builtin_scripts bridge_prepare_system)
add_dependencies(lua_module_builder_sync_docs bridge_prepare_system)
add_dependencies(littlefs_system_bin bridge_prepare_system)
