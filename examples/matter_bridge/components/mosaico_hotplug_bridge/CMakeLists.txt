# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "mosaico_hotplug_bridge.c"
         "mosaico_hotplug_devices.cpp"
         "accessories/interact/interact_accessory.cpp"
         "accessories/interact/interact_inputs.c"
         "accessories/camera/camera_accessory.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private" "accessories/interact"
    REQUIRES matter_bridge mosaico_module_mgr mosaico_matter_camera
    PRIV_REQUIRES esp_timer freertos heap nvs_flash
                  esp-mosaico-bsp led_strip soc esp_adc esp_driver_gpio
)
