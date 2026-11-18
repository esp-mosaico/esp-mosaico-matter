# XiaoZhi voice session for the Matter hub.
# espressif/esp_xiaozhi owns the server session, transport, and MCP dispatch.
# This component owns capture, playback, WakeNet, and the Matter tools.
# The Mosaic UI and Wi-Fi manager stay where they are.

idf_component_register(
    SRCS
        "xiaozhi_voice.cpp"
        "hub_audio.cpp"
        "hub_wake.cpp"
        "hub_matter_tools.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        espressif__esp_xiaozhi
        espressif__esp-sr
        espressif__esp_audio_codec
        espressif__esp_audio_effects
        audio_hub
        local_fabric
        console
        nvs_flash
        esp_event
        esp_netif
        esp_timer
        esp_wifi
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=missing-field-initializers
    -Wno-missing-field-initializers
)
