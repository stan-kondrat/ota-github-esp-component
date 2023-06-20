idf_component_register(SRCS "src/ota_github.c"
                    INCLUDE_DIRS src
                    PRIV_REQUIRES "log" "esp_http_client")
