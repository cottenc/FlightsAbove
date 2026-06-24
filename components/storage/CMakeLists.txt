idf_component_register(
    SRCS "storage.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        app_config
        freertos
        nvs_flash
)
