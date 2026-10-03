idf_component_register(SRCS "model.c" "demo.c" INCLUDE_DIRS "include"
                       REQUIRES logic settings weather
                       PRIV_REQUIRES gcal gauth ical net json log freertos esp_hw_support heap)
