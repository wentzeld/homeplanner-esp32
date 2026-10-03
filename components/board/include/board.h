// Board support for the Elecrow CrowPanel Advance ESP32-P4 10.1" (V1.2 pinout):
// power rails, MIPI-DSI EK79007 display, backlight, GT911 touch, LVGL port.
#pragma once

#include "esp_err.h"
#include "lvgl.h"

#define BOARD_LCD_H_RES 1024
#define BOARD_LCD_V_RES 600

// Power up the display, start LVGL and attach touch. The backlight stays off until
// board_set_backlight() is called, so the first frame can be drawn first.
esp_err_t board_init(void);

// 0 = off, 100 = full brightness.
esp_err_t board_set_backlight(int percent);

lv_display_t *board_display(void);
lv_indev_t *board_touch(void);
