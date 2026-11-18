# CONFIG_* is unavailable until project(). Mirror IDF's defaults/target
# overlay order here so component selection also works on a clean checkout.
set(MATTER_HUB_SDKCONFIG "${CMAKE_SOURCE_DIR}/sdkconfig")
if(SDKCONFIG)
    get_filename_component(MATTER_HUB_SDKCONFIG "${SDKCONFIG}" ABSOLUTE
        BASE_DIR "${CMAKE_SOURCE_DIR}")
endif()

if(NOT SDKCONFIG_DEFAULTS)
    if(NOT "$ENV{SDKCONFIG_DEFAULTS}" STREQUAL "")
        set(SDKCONFIG_DEFAULTS "$ENV{SDKCONFIG_DEFAULTS}")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
        set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
    endif()
endif()

set(_hub_target "${IDF_TARGET}")
if(NOT "$ENV{IDF_TARGET}" STREQUAL "")
    set(_hub_target "$ENV{IDF_TARGET}")
endif()
if(NOT _hub_target)
    # IDF guesses the target from sdkconfig before consulting defaults.
    foreach(_config IN ITEMS "${MATTER_HUB_SDKCONFIG}" ${SDKCONFIG_DEFAULTS})
        get_filename_component(_config "${_config}" ABSOLUTE
            BASE_DIR "${CMAKE_SOURCE_DIR}")
        if(EXISTS "${_config}")
            file(STRINGS "${_config}" _target_line
                REGEX "^CONFIG_IDF_TARGET=\"[^\"]+\"$")
            if(_target_line MATCHES "^CONFIG_IDF_TARGET=\"([^\"]+)\"$")
                set(_hub_target "${CMAKE_MATCH_1}")
                break()
            endif()
        endif()
    endforeach()
endif()
if(NOT _hub_target)
    set(_hub_target esp32s31)
endif()

set(_hub_config_files)
foreach(_default IN LISTS SDKCONFIG_DEFAULTS)
    get_filename_component(_default "${_default}" ABSOLUTE
        BASE_DIR "${CMAKE_SOURCE_DIR}")
    list(APPEND _hub_config_files "${_default}" "${_default}.${_hub_target}")
endforeach()
list(APPEND _hub_config_files "${MATTER_HUB_SDKCONFIG}")

set(MATTER_HUB_AGENT claw)
foreach(_config IN LISTS _hub_config_files)
    if(EXISTS "${_config}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_config}")
        file(STRINGS "${_config}" _agent_lines
            REGEX "^CONFIG_APP_MATTER_HUB_(CLAW|XIAOZHI|NO_AGENT)=y$")
        foreach(_line IN LISTS _agent_lines)
            if(_line MATCHES "^CONFIG_APP_MATTER_HUB_(CLAW|XIAOZHI|NO_AGENT)=y$")
                string(TOLOWER "${CMAKE_MATCH_1}" MATTER_HUB_AGENT)
            endif()
        endforeach()
    endif()
endforeach()

# kconfgen loads SDKCONFIG_DEFAULTS first, then overlays existing sdkconfig
# entries on top. When the selected agent changes, strip variant-managed keys
# from sdkconfig so the new SDKCONFIG_DEFAULTS can take effect.
set(MATTER_HUB_AGENT_STAMP "${CMAKE_BINARY_DIR}/matter_hub_agent.stamp")
set(MATTER_HUB_PREV_AGENT "")
if(EXISTS "${MATTER_HUB_AGENT_STAMP}")
    file(STRINGS "${MATTER_HUB_AGENT_STAMP}" MATTER_HUB_PREV_AGENT LIMIT_COUNT 1)
endif()
# A missing stamp means a new build directory, not an agent change. Preserve
# explicit sdkconfig values across fullclean and independent build directories.
if(MATTER_HUB_PREV_AGENT AND
        NOT MATTER_HUB_PREV_AGENT STREQUAL "${MATTER_HUB_AGENT}")
    if(EXISTS "${MATTER_HUB_SDKCONFIG}")
        set(_agent_keys "")
        foreach(_agent IN ITEMS claw no_agent xiaozhi)
            set(_f "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.${_agent}")
            if(EXISTS "${_f}")
                file(STRINGS "${_f}" _lines REGEX "^(# )?CONFIG_[A-Za-z0-9_]+")
                foreach(_line IN LISTS _lines)
                    if(_line MATCHES "^(# )?(CONFIG_[A-Za-z0-9_]+)(=| is not set$)")
                        set(_key "${CMAKE_MATCH_2}")
                        if(NOT _key MATCHES "^CONFIG_APP_MATTER_HUB_")
                            list(APPEND _agent_keys "${_key}")
                        endif()
                    endif()
                endforeach()
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _agent_keys)
        if(_agent_keys)
            list(JOIN _agent_keys "|" _key_re)
            file(READ "${MATTER_HUB_SDKCONFIG}" _sdkconfig_content)
            string(REGEX REPLACE
                "\r?\n(# default:\r?\n)?((${_key_re})=|# (${_key_re}) is not set)[^\r\n]*"
                "" _sdkconfig_updated "\n${_sdkconfig_content}")
            string(REGEX REPLACE "^\r?\n" "" _sdkconfig_updated "${_sdkconfig_updated}")
            if(NOT _sdkconfig_updated STREQUAL "${_sdkconfig_content}")
                file(WRITE "${MATTER_HUB_SDKCONFIG}" "${_sdkconfig_updated}")
            endif()
        endif()
    endif()
    file(REMOVE "${CMAKE_BINARY_DIR}/config/sdkconfig.json")
endif()
if(NOT MATTER_HUB_PREV_AGENT STREQUAL "${MATTER_HUB_AGENT}")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}")
    file(WRITE "${MATTER_HUB_AGENT_STAMP}" "${MATTER_HUB_AGENT}\n")
endif()

set(_hub_ui_apps "hub;matter;thread_br;settings;setup_center;welcome")
if(MATTER_HUB_AGENT STREQUAL "xiaozhi")
    list(INSERT _hub_ui_apps 1 xiaozhi)
elseif(MATTER_HUB_AGENT STREQUAL "claw")
    list(INSERT _hub_ui_apps 1 ai_create)
endif()
set(MOSAICO_UI_APP_MANIFESTS "${_hub_ui_apps}" CACHE STRING "" FORCE)

if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.${MATTER_HUB_AGENT}")
    list(APPEND SDKCONFIG_DEFAULTS
        "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.${MATTER_HUB_AGENT}")
endif()
if(NOT MATTER_HUB_AGENT STREQUAL "xiaozhi")
    list(APPEND EXCLUDE_COMPONENTS xiaozhi_voice)
endif()
