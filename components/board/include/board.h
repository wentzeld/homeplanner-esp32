// Board support for the Elecrow CrowPanel Advance ESP32-P4 10.1" (V1.2 pinout):
// power rails, MIPI-DSI EK79007 display, backlight, GT911 touch, LVGL port.
#pragma once
#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#define BOARD_LCD_H_RES 1024
#define BOARD_LCD_V_RES 600

// Power up the display, start LVGL and attach touch. The backlight stays off until
// board_set_backlight() is called, so the first frame can be drawn first.
esp_err_t board_init(void);

// 0 = off, 100 = full brightness.
esp_err_t board_set_backlight(int percent);
// Keep the backlight off whatever board_set_backlight() asks (during software updates, whose flash
// writes would make the screen flicker); false restores the last requested brightness.
void board_backlight_hold(bool off);

lv_display_t *board_display(void);
lv_indev_t *board_touch(void);
