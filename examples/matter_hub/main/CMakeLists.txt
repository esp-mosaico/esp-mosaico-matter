# Keep the optional non-GSP status display separate from the Mosaic UI adapters.
set(main_exclude_srcs)
set(main_ldfragments)
set(main_embed_files)
set(main_priv_requires
    camera_buffer_pool
    app_settings_service
    app_system_config
    audio_hub
    claw_hw_registry
    claw_event_router
    cmd_wifi
    display_service
    esp_lvgl_present
    espressif__esp_display_present
    esp_app_format
    esp_hw_support
    esp_wifi
    fatfs
    wear_levelling
    espressif__cjson
    espressif__bq27220
    gen_bmgr_codes
    local_fabric
    claw_matter_controller
    http_server
    nvs_flash
    gmf_core
)

if(CONFIG_ESP_BOARD_ESP_MOSAICO)
    list(APPEND main_ldfragments "linker.lf")
endif()

if(NOT CONFIG_APP_CLAW_MOSAIC_GSP_ENABLE)
    list(APPEND main_exclude_srcs
        "platform/mosaic_battery_platform.c"
        "platform/mosaic_button_platform.c"
        "platform/mosaic_settings_platform.c")
    list(APPEND main_embed_files "fonts/clock.ttf")
else()
    list(APPEND main_exclude_srcs "app_status_display.c")
endif()

idf_component_register(
    SRC_DIRS "." "platform"
    EXCLUDE_SRCS ${main_exclude_srcs}
    INCLUDE_DIRS "." "platform"
    LDFRAGMENTS ${main_ldfragments}
    EMBED_FILES ${main_embed_files}
    PRIV_REQUIRES ${main_priv_requires}
)

# CONFIG_* is unavailable during IDF's early dependency expansion. These
# components are selected by the board/project manifests; attach their include
# paths and link dependencies here, after configuration has been loaded.
if(CONFIG_ESP_BOARD_ESP_MOSAICO)
    idf_component_optional_requires(PRIVATE
        mosaico_bsp_bmgr_bridge mosaico_camera_service mosaico_boot_splash)
endif()

if(CONFIG_APP_THREAD_BORDER_ROUTER_ENABLE)
    idf_component_optional_requires(PRIVATE thread_border_router)
endif()

if(CONFIG_APP_CLAW_MOSAIC_GSP_ENABLE)
    idf_component_optional_requires(PRIVATE weather_service update_check_service)
endif()
