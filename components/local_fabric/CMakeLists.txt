set(FABRIC_CORE_SRCS
    "fabric_core.cpp"
    "fabric_core_chip.cpp"
    "fabric_core_operations.cpp"
    "fabric_core_store.cpp"
    "fabric_core_subscription.cpp"
)

set(FABRIC_MANAGER_SRCS)
if(CONFIG_FABRIC_MANAGER_ENABLE)
    list(APPEND FABRIC_MANAGER_SRCS
        "fabric_manager.cpp"
        "data_model/data_model_json.cpp"
        "data_model/data_model_report.cpp"
        "data_model/data_model_topology.cpp"
        "data_model/data_model_model.cpp"
        "data_model/data_model_store.cpp"
        "data_model/clusters/on_off/on_off.cpp"
        "data_model/clusters/level_control/level_control.cpp"
        "data_model/clusters/color_control/color_control.cpp"
        "data_model/clusters/temperature_measurement/temperature_measurement.cpp"
        "data_model/clusters/relative_humidity_measurement/relative_humidity_measurement.cpp"
        "data_model/clusters/illuminance_measurement/illuminance_measurement.cpp"
        "data_model/clusters/occupancy_sensing/occupancy_sensing.cpp"
        "data_model/clusters/boolean_state/boolean_state.cpp"
        "data_model/clusters/power_source/power_source.cpp"
        "data_model/clusters/thermostat/thermostat.cpp"
        "data_model/clusters/fan_control/fan_control.cpp"
        "data_model/clusters/rvc_mode/rvc_mode.cpp"
        "data_model/clusters/rvc_operational_state/rvc_operational_state.cpp"
    )
endif()

idf_component_register(
    SRCS ${FABRIC_CORE_SRCS} ${FABRIC_MANAGER_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private" "data_model"
    LDFRAGMENTS "linker.lf"
    REQUIRES espressif__cjson
    PRIV_REQUIRES esp_matter nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
