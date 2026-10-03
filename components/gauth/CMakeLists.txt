idf_component_register(SRCS "oauth.c" "gauth.c" INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES json mbedtls net settings log freertos heap)
