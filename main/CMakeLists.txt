# main 组件的 CMakeLists.txt
idf_component_register(
    SRCS
        "main.c"
        "doubao_api.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_http_client
        esp-tls
        cJSON
        nvs_flash
        esp_wifi
)
