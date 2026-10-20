idf_component_register(
    SRCS "src/claw_matter_bridge_model.cpp"
         "src/claw_matter_bridge.cpp"
         "src/claw_matter_bridge_hardware.cpp"
         "src/claw_matter_bridge_lua.cpp"
         "src/claw_matter_bridge_api.cpp"
         "src/claw_matter_bridge_lua_json.cpp"
    INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "private"
    REQUIRES matter_bridge
    PRIV_REQUIRES app_claw cap_lua claw_hw_registry cjson
                  esp_driver_gpio esp_driver_ledc esp_timer led_strip heap)
