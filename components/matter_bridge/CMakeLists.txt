set(BRIDGE_CORE_SRCS
    "core/bridge_core.cpp"
    "core/bridge_core_commissioning.cpp"
)

set(BRIDGE_DEV_MNGR_SRCS
    "dev_mngr/bridge_dev_mngr.cpp"
    "dev_mngr/bridge_dev_mngr_attributes.cpp"
    "dev_mngr/bridge_dev_mngr_operations.cpp"
    "dev_mngr/bridge_dev_mngr_profiles.cpp"
    "dev_mngr/bridge_dev_mngr_store.cpp"
    "dev_mngr/clusters/bridged_device_basic_information/bridged_device_basic_information.cpp"
    "dev_mngr/clusters/occupancy_sensing/occupancy_sensing.cpp"
)

idf_component_register(
    SRCS ${BRIDGE_CORE_SRCS} ${BRIDGE_DEV_MNGR_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private" "dev_mngr"
    REQUIRES esp_matter
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif
)
