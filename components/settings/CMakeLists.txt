idf_component_register(SRCS "settings_json.c" "settings_store.c"
                       INCLUDE_DIRS "include"
                       REQUIRES logic
                       PRIV_REQUIRES json tz nvs_flash log)
