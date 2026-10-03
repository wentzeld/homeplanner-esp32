idf_component_register(SRCS "web.c" "web_auth.c" "dns.c"
                       INCLUDE_DIRS "include"
                       EMBED_TXTFILES "page.html"
                       REQUIRES settings
                       PRIV_REQUIRES esp_http_server net ical gauth gcal update tz json lwip log logic nvs_flash mbedtls esp_hw_support)
