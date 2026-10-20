idf_component_register(
    SRCS "camera_buffer_pool.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_mm heap freertos log
)
