idf_component_register(
    SRCS "thread_border_router.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    REQUIRES esp_common
    PRIV_REQUIRES openthread esp_netif esp_event esp_wifi esp_driver_gpio
        esp_driver_uart nvs_flash vfs mdns mosaico_module_mgr
)
if(NOT CONFIG_OPENTHREAD_ENABLED OR NOT CONFIG_OPENTHREAD_RADIO_SPINEL_UART)
    message(FATAL_ERROR
        "Thread Border Router requires the Mosaico board overlay. From examples/matter_hub run "
        "idf.py bmgr -c \"boards;../../third_party/esp-mosaico-claw/boards\" -b esp_mosaico, "
        "then reconfigure sdkconfig with the board defaults to enable OpenThread over UART.")
endif()
if(CONFIG_ENABLE_ROUTE_HOOK)
    message(FATAL_ERROR
        "Set CONFIG_ENABLE_ROUTE_HOOK=n; IDF's Thread border router supplies the IPv6 route hooks")
endif()
if(CONFIG_USE_MINIMAL_MDNS)
    message(FATAL_ERROR
        "Set CONFIG_USE_MINIMAL_MDNS=n; Matter and the Thread SRP proxy must share platform mDNS")
endif()
