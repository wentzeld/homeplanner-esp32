idf_component_register(SRCS "ical_parse.c" "ical_expand.c"
                       INCLUDE_DIRS "include"
                       REQUIRES logic tz
                       PRIV_REQUIRES heap)
