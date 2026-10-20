set(claw_matter_sources
    "src/claw_matter_controller.cpp"
    "src/claw_matter_api.cpp"
)

if(CONFIG_CLAW_MATTER_LUA_ENABLE)
    list(APPEND claw_matter_sources "src/claw_matter_lua.cpp")
endif()

idf_component_register(
    SRCS
        ${claw_matter_sources}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "private"
    REQUIRES
        local_fabric
    PRIV_REQUIRES
        app_claw
        cap_lua
)
