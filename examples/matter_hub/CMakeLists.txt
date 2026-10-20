cmake_minimum_required(VERSION 3.16)

get_filename_component(ESP_CLAW_MATTER_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(ESP_MOSAICO_UPSTREAM
    "${ESP_CLAW_MATTER_ROOT}/third_party/esp-mosaico-claw")
list(APPEND EXTRA_COMPONENT_DIRS
    "${ESP_MOSAICO_UPSTREAM}/components"
    "${ESP_MOSAICO_UPSTREAM}/third-party/esp-claw/components/common/esp_video"
    "${ESP_CLAW_MATTER_ROOT}/components")

# The hub is controller-only; server bridge components belong to
# examples/matter_bridge.
# mosaic_ui is the submodule copy under third_party/esp-mosaico-claw; this
# project uses the local mosaico_ui component instead. After the rename they
# are distinct IDF component names, so the extra-component-dir scan would
# otherwise register both.
list(APPEND EXCLUDE_COMPONENTS matter_bridge claw_matter_bridge mosaic_ui)

# mosaico_ui is shared with examples/matter_bridge; tell it which optional
# backends this project needs (see
# components/mosaico_ui/CMakeLists.txt). Must
# be CACHE variables: mosaico_ui/CMakeLists.txt is processed in a separate
# ESP-IDF component-registration scope that does not see a plain set() here.
set(MOSAICO_UI_WITH_MATTER_CONTROLLER ON CACHE BOOL "" FORCE)
set(MOSAICO_UI_WITH_THREAD_BR ON CACHE BOOL "" FORCE)

include("${ESP_MOSAICO_UPSTREAM}/tools/cmake/esp_idf_patch.cmake")
include("${ESP_MOSAICO_UPSTREAM}/tools/cmake/flash_partition_defaults.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(
    BOOTLOADER_EXTRA_COMPONENT_DIRS
    "${ESP_MOSAICO_UPSTREAM}/components/mosaico_boot_splash"
    APPEND
)
# ESP-Matter 1.6.0 has a handful of deliberately ignored certificate helper
# results that GCC 15 diagnoses as errors under the project's global -Werror.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-result" APPEND)

# FreeType's optional PNG-glyph and external zlib backends can auto-detect
# host libraries during cross-compilation. Our fonts do not require them;
# override cached discoveries as well. This does not disable GSP PNG images.
set(FT_DISABLE_PNG ON CACHE BOOL "Disable FreeType PNG bitmap glyph support" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "Disable FreeType external zlib support" FORCE)

project(matter_hub VERSION 0.1.0)

if(CONFIG_CAMERA_BUFFER_POOL_ENABLE)
    include("${CMAKE_CURRENT_LIST_DIR}/components/camera_buffer_pool/cmake/integrate.cmake")
endif()

# ESP-Matter 1.6.0 still refers to PeerAddress through the old chip namespace.
# ESP-IDF 6.1 builds as C++26, where that alias is no longer visible. Apply the
# compatibility repair after project(), once the component manager has fetched
# managed dependencies on a clean checkout.
edge_agent_patch_file_replace(
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_matter/components/esp_matter_controller/commands/esp_matter_controller_pairing_command.cpp"
    "    PeerAddress peerAddress = PeerAddress::UDP(nodeData.ipAddress[0], port, interfaceId);"
    "    Transport::PeerAddress peerAddress = Transport::PeerAddress::UDP(nodeData.ipAddress[0], port, interfaceId);"
    "esp_matter_controller_peer_address_namespace"
)

# The controller client contains a ~143 KiB DeviceCommissioner singleton. Keeping
# it in internal .bss leaves too little DMA-capable heap for esp_wifi_init() on
# ESP32-S31, while the singleton is only first used after PSRAM initialization.
set(EDGE_AGENT_MATTER_CONTROLLER_CLIENT_HEADER
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_matter/components/esp_matter_controller/core/esp_matter_controller_client.h")
edge_agent_patch_file_replace(
    "${EDGE_AGENT_MATTER_CONTROLLER_CLIENT_HEADER}"
    "#include <esp_log.h>"
    "#include <esp_attr.h>\n#include <esp_log.h>"
    "esp_matter_controller_client_ext_ram_attr_include"
)
edge_agent_patch_file_replace(
    "${EDGE_AGENT_MATTER_CONTROLLER_CLIENT_HEADER}"
    "        static matter_controller_client s_instance;"
    "        static EXT_RAM_BSS_ATTR matter_controller_client s_instance;"
    "esp_matter_controller_client_ext_ram_bss"
)

# A pending BTP acknowledgement timer can run after NimBLE reports a GAP
# disconnect and deletes its peer record. ESP-Matter's controller write path
# otherwise passes that NULL peer to peer_chr_find_uuid() and crashes.
edge_agent_patch_file_replace(
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_matter/connectedhomeip/connectedhomeip/src/platform/ESP32/nimble/BLEManagerImpl.cpp"
    [=[    const struct peer * peer = peer_find(conId);

    chr = peer_chr_find_uuid(peer, (ble_uuid_t *) (&ShortUUID_CHIPoBLEService), (ble_uuid_t *) (&UUID128_CHIPoBLEChar_RX));]=]
    [=[    const struct peer * peer = peer_find(conId);
    if (peer == nullptr)
    {
        ChipLogError(Ble, "Cannot send write: peer for connection %u is gone", conId);
        return BLE_ERROR_GATT_WRITE_FAILED;
    }

    chr = peer_chr_find_uuid(peer, (ble_uuid_t *) (&ShortUUID_CHIPoBLEService), (ble_uuid_t *) (&UUID128_CHIPoBLEChar_RX));]=]
    "esp_matter_nimble_send_write_disconnected_peer"
)

# Espressif mDNS's query API encodes the service argument as one DNS label, so
# CHIP's `<subtype>._sub.<service>` browse names cannot be emitted correctly.
# Browse the base service instead; SetUpCodePairer applies the discriminator,
# vendor, and product filters to each result before opening PASE.
edge_agent_patch_file_replace(
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_matter/connectedhomeip/connectedhomeip/src/platform/ESP32/ESP32DnssdImpl.cpp"
    [=[    CHIP_ERROR error = CHIP_NO_ERROR;
    mdns_search_once_t * queryHandle =
        mdns_query_async_new(NULL, type, GetProtocolString(protocol), MDNS_TYPE_PTR, kTimeoutMilli, kMaxResults, MdnsQueryNotifier);]=]
    [=[    CHIP_ERROR error = CHIP_NO_ERROR;
    const char * queryType = type;
    const char * subtypeSeparator = strstr(type, "._sub.");
    if (subtypeSeparator != nullptr)
    {
        queryType = subtypeSeparator + strlen("._sub.");
    }
    mdns_search_once_t * queryHandle =
        mdns_query_async_new(NULL, queryType, GetProtocolString(protocol), MDNS_TYPE_PTR, kTimeoutMilli, kMaxResults,
                             MdnsQueryNotifier);]=]
    "esp_matter_mdns_subtype_browse"
)

set(EDGE_AGENT_FATFS_IMAGE_ROOT "${ESP_MOSAICO_UPSTREAM}/fatfs_image")
set(EDGE_AGENT_FATFS_SYSTEM_SOURCE_ROOT "${EDGE_AGENT_FATFS_IMAGE_ROOT}/system")
set(EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT "${CMAKE_BINARY_DIR}/system_fs_image")
set(EDGE_AGENT_BOARD_GENERATED_CMAKELISTS "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes/CMakeLists.txt")

set(EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS "")

if(EXISTS "${EDGE_AGENT_BOARD_GENERATED_CMAKELISTS}")
    file(STRINGS "${EDGE_AGENT_BOARD_GENERATED_CMAKELISTS}" EDGE_AGENT_BOARD_PATH_LINE
        REGEX "^message\\(STATUS \"Board Path: .+\"\\)$"
    )

    if(EDGE_AGENT_BOARD_PATH_LINE)
        list(GET EDGE_AGENT_BOARD_PATH_LINE 0 EDGE_AGENT_BOARD_PATH_LINE)
        string(REGEX REPLACE
            "^message\\(STATUS \"Board Path: (.*)\"\\)$"
            "\\1"
            EDGE_AGENT_BOARD_PATH
            "${EDGE_AGENT_BOARD_PATH_LINE}"
        )

        set(EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT "${EDGE_AGENT_BOARD_PATH}/fatfs_image")

        if(EXISTS "${EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT}")
            list(APPEND EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT}"
                        "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
            )
        endif()
    endif()
endif()

# Prepare the read-only system partition build tree. Its existing .recovery
# defaults seed a fresh NAND at boot; the former storage image is intentionally
# not reproduced because the removed NOR space must remain unpartitioned and
# the fixed-size SYSTEM image has no room for a second copy of DATA skills.
# Skill/Lua syncs can also write into this tree before it is packed.
add_custom_target(edge_agent_prepare_system_fs_image ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${EDGE_AGENT_FATFS_SYSTEM_SOURCE_ROOT}"
            "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    ${EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS}
    VERBATIM
)

littlefs_create_partition_image(system "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}" FLASH_IN_PROJECT)

# Skill / Lua resources are baked into the read-only system partition so DATA
# recovery does not depend on a writable copy surviving.
skill_builder_configure_skill_sync(
    TARGET littlefs_system_bin
    SKILL_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/skills"
)

lua_module_builder_configure_builtin_lua_sync(
    TARGET littlefs_system_bin
    BUILTIN_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/scripts/builtin"
    DOCS_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/scripts/docs"
)

add_dependencies(skill_builder_sync_skills edge_agent_prepare_system_fs_image)
add_dependencies(lua_module_builder_sync_builtin_scripts edge_agent_prepare_system_fs_image)
add_dependencies(lua_module_builder_sync_docs edge_agent_prepare_system_fs_image)
add_dependencies(littlefs_system_bin edge_agent_prepare_system_fs_image)
