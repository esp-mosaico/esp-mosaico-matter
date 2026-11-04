set(FRONTEND_SOURCE_DIR        "${CMAKE_CURRENT_LIST_DIR}/frontend_source")
set(FRONTEND_DIST_HTML_GZ      "${FRONTEND_SOURCE_DIR}/dist/index.html.gz")
set(FRONTEND_PUBLIC_FAVICON_SVG "${FRONTEND_SOURCE_DIR}/public/favicon.svg")

if(NOT EXISTS "${FRONTEND_DIST_HTML_GZ}")
    message(FATAL_ERROR
        "\n"
        "Frontend bundle not found: ${FRONTEND_DIST_HTML_GZ}\n"
        "Build the SolidJS UI before running idf.py build, e.g.:\n"
        "    cd ${FRONTEND_SOURCE_DIR}\n"
        "    pnpm install --frozen-lockfile\n"
        "    pnpm run build\n")
endif()

set(http_server_srcs
    "http_server_core.c"
    "http_server_assets.c"
    "http_server_config_api.c"
    "http_server_status_api.c"
    "http_server_files_api.c"
    "http_server_json.c"
    "http_server_utils.c"
)

set(http_server_requires
    espressif__cjson
    esp_http_server
    esp_psram
    esp_timer
    heap
)

set(http_server_include_dirs "include")

if(CONFIG_HTTP_SERVER_CLAW_ENABLE)
    list(APPEND http_server_srcs
        "claw/http_server_claw.c"
        "claw/http_server_capabilities_api.c"
        "claw/http_server_lua_modules_api.c"
        "claw/http_server_wechat_api.c")
    list(APPEND http_server_include_dirs "claw/include")
    if(CONFIG_APP_CLAW_CAP_IM_LOCAL AND CONFIG_HTTPD_WS_SUPPORT)
        list(APPEND http_server_srcs "claw/http_server_webim_api.c")
    endif()
    if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER)
        list(APPEND http_server_srcs "claw/http_server_lua_app_api.c")
    endif()
endif()

# Set by each example before project(). The backend components are selected
# by the application's manifest; attach their dependencies after registration
# because this cache setting is unavailable in early dependency expansion.
if(HTTP_SERVER_MATTER_BACKEND STREQUAL "controller")
    list(APPEND http_server_srcs "http_server_matter_api.c")
    set(http_server_matter_role controller)
elseif(HTTP_SERVER_MATTER_BACKEND STREQUAL "bridge")
    list(APPEND http_server_srcs "http_server_matter_bridge_api.c")
    set(http_server_matter_role bridge)
else()
    list(APPEND http_server_srcs "http_server_matter_none.c")
    set(http_server_matter_role none)
endif()

idf_component_register(
    SRCS
        ${http_server_srcs}
    INCLUDE_DIRS
        ${http_server_include_dirs}
    EMBED_FILES
        "${FRONTEND_DIST_HTML_GZ}"
        "${FRONTEND_PUBLIC_FAVICON_SVG}"
    REQUIRES
        ${http_server_requires}
)

# CONFIG_* is unavailable during early dependency expansion. The conditional
# manifest selects Claw dependencies; attach them after configuration is loaded.
if(CONFIG_HTTP_SERVER_CLAW_ENABLE)
    idf_component_optional_requires(PRIVATE app_claw)
    if(CONFIG_APP_CLAW_CAP_IM_WECHAT)
        idf_component_optional_requires(PRIVATE cap_im_platform)
    endif()
    if(CONFIG_APP_CLAW_CAP_IM_LOCAL AND CONFIG_HTTPD_WS_SUPPORT)
        idf_component_optional_requires(PRIVATE cap_im_local)
    endif()
    if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER)
        idf_component_optional_requires(PRIVATE lua_module_http_server)
    endif()
endif()

if(HTTP_SERVER_MATTER_BACKEND STREQUAL "controller")
    idf_component_optional_requires(PUBLIC local_fabric)
elseif(HTTP_SERVER_MATTER_BACKEND STREQUAL "bridge")
    idf_component_optional_requires(PUBLIC matter_bridge)
endif()

# Rebuild the component whenever the bundled UI asset changes on disk.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FRONTEND_DIST_HTML_GZ}")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HTTP_SERVER_MATTER_ROLE="${http_server_matter_role}")
