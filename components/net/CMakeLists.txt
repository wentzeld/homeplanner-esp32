idf_component_register(SRCS "net.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif esp_event
                       PRIV_REQUIRES esp_wifi nvs_flash lwip esp_http_client esp-tls mbedtls espressif__esp_hosted)
