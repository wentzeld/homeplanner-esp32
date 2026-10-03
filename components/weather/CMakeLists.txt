idf_component_register(SRCS "weather.c" "weather_parse.c" INCLUDE_DIRS "include" REQUIRES esp_common PRIV_REQUIRES net json log)
