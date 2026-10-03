idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES board net settings web ui gauth weather model esp_psram spi_flash)
