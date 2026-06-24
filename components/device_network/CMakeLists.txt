idf_component_register(
    SRCS "device_network.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        app_config
        app_update
        esp_app_format
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        freertos
        heap
        setup_portal
        storage
)
