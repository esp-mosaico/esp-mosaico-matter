idf_component_register(SRCS "main.cpp" "bridge_battery.c" "router_config.c" "router_defaults.c" "bridge_memory.c" "llm_memory.c" INCLUDE_DIRS "."
    LDFRAGMENTS "linker.lf"
    PRIV_REQUIRES claw_matter_bridge app_claw app_config http_server
        gen_bmgr_codes esp_board_manager nvs_flash esp_wifi esp_netif
        littlefs claw_core claw_event_router claw_hw_registry heap)

# CONFIG_* is unavailable during IDF's early dependency expansion. The
# top-level CMakeLists adds these components; attach their
# include paths and link dependencies here, after configuration is loaded.
if(CONFIG_APP_CLAW_MOSAIC_GSP_ENABLE)
    idf_component_optional_requires(PRIVATE
        mosaico_hotplug_bridge mosaico_matter_camera mosaico_ui)
endif()

# CONFIG_* is unavailable during IDF's early dependency expansion, so this
# board-only component (see the esp_mosaico board amend) is attached here,
# after configuration has been loaded -- matching matter_hub's main.
if(CONFIG_ESP_BOARD_ESP_MOSAICO)
    idf_component_optional_requires(PRIVATE mosaico_bsp_bmgr_bridge)
endif()

# app_claw fixes the router stack at 8 KiB; override its public initialization
# configuration for this example without changing the upstream component.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=claw_event_router_init"
    "-Wl,--undefined=__wrap_claw_event_router_init")

# Sample heap at LLM HTTP boundaries without modifying the shared transport.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=claw_llm_http_post_json"
    "-Wl,--undefined=__wrap_claw_llm_http_post_json")
