// Host stand-in for ESP-IDF's esp_err.h (only what the pure parsers' headers need).
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND 0x105
