idf_component_register(SRCS "update_logic.c" "update.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES json app_update esp_https_ota esp_http_client mbedtls bootloader_support
                                     esp_timer net nvs_flash log freertos heap esp_system)
