idf_component_register(SRCS "ui_basic.c" "ui_week.c" "ui_form.c" "ui_menu.c" INCLUDE_DIRS "include"
                       REQUIRES board settings
                       PRIV_REQUIRES model logic weather heap web)
