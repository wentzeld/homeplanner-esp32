idf_component_register(SRCS "board.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd esp_driver_ledc esp_driver_i2c esp_driver_gpio
                       PRIV_REQUIRES esp_hw_support)
