# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "mosaico_hotplug_bridge.c"
         "mosaico_hotplug_devices.c"
         "accessories/interact/interact_accessory.c"
         "accessories/interact/interact_lua.c"
         "accessories/camera/camera_accessory.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private" "accessories/interact"
    EMBED_TXTFILES
        "accessories/interact/mosaico_interact_led.lua"
    REQUIRES mosaico_module_mgr mosaico_matter_camera
    PRIV_REQUIRES claw_matter_bridge cap_lua cjson esp_timer freertos heap
                  esp-mosaico-bsp led_strip soc
)
