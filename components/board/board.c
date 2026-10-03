// Board bring-up for the CrowPanel Advance 10.1" (ESP32-P4). Values follow Elecrow's V1.2
// examples (Lesson07 display, Lesson09 touch); see README for sources.
#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "board";

// --- Pins / constants -------------------------------------------------------------------------
#define PIN_BACKLIGHT 31
#define BACKLIGHT_PWM_HZ 30000
#define PIN_TOUCH_SDA 45
#define PIN_TOUCH_SCL 46
#define PIN_TOUCH_RST 40
#define PIN_TOUCH_INT 42
#define LDO_CHAN_MIPI_PHY 3  // 2.5 V for the MIPI-DSI PHY
#define LDO_CHAN_IO 4        // 3.3 V rail used by the display/touch
#define DSI_LANES 2
#define DSI_LANE_MBPS 900
#define DPI_CLOCK_MHZ 51

static esp_ldo_channel_handle_t s_ldo_phy, s_ldo_io;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_io_handle_t s_dbi_io;
static esp_lcd_panel_handle_t s_panel;
static i2c_master_bus_handle_t s_i2c;
static esp_lcd_touch_handle_t s_touch;
static lv_display_t *s_disp;
static lv_indev_t *s_indev;

static esp_err_t power_init(void) {
    esp_ldo_channel_config_t phy = {.chan_id = LDO_CHAN_MIPI_PHY, .voltage_mv = 2500};
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&phy, &s_ldo_phy), TAG, "LDO3");
    esp_ldo_channel_config_t io = {.chan_id = LDO_CHAN_IO, .voltage_mv = 3300};
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&io, &s_ldo_io), TAG, "LDO4");
    return ESP_OK;
}

static esp_err_t backlight_init(void) {
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_11_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BACKLIGHT_PWM_HZ,
        .clk_cfg = LEDC_USE_PLL_DIV_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {
        .gpio_num = PIN_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    return ledc_channel_config(&channel);
}

esp_err_t board_set_backlight(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    // Same mapping as Elecrow's example: a floor so low values still light the panel.
    uint32_t duty = percent == 0 ? 0 : (uint32_t)(percent * 18 + 200);
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static esp_err_t panel_init(void) {
    const esp_lcd_dsi_bus_config_t bus = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus, &s_dsi_bus), TAG, "DSI bus");
    const esp_lcd_dbi_io_config_t dbi = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi, &s_dbi_io), TAG, "DBI io");

    const esp_lcd_dpi_panel_config_t dpi = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DPI_CLOCK_MHZ,
        .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = BOARD_LCD_H_RES,
            .v_size = BOARD_LCD_V_RES,
            .hsync_back_porch = 160,
            .hsync_pulse_width = 70,
            .hsync_front_porch = 160,
            .vsync_back_porch = 23,
            .vsync_pulse_width = 10,
            .vsync_front_porch = 12,
        },
        .flags.use_dma2d = true,
    };
    ek79007_vendor_config_t vendor = {.mipi_config = {.dsi_bus = s_dsi_bus, .dpi_config = &dpi}};
    const esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ek79007(s_dbi_io, &dev, &s_panel), TAG, "EK79007");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    return esp_lcd_panel_init(s_panel);
}

static esp_err_t lvgl_init(void) {
    const lvgl_port_cfg_t port = {
        .task_priority = 4,
        .task_stack = 16 * 1024,
        .task_affinity = -1,
        .task_max_sleep_ms = 10,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port), TAG, "LVGL port");
    const lvgl_port_display_cfg_t disp = {
        .io_handle = s_dbi_io,
        .panel_handle = s_panel,
        .control_handle = s_panel,
        .buffer_size = BOARD_LCD_H_RES * BOARD_LCD_V_RES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_spiram = true, .sw_rotate = false, .swap_bytes = false},
    };
    const lvgl_port_display_dsi_cfg_t dsi = {.flags = {.avoid_tearing = false}};
    s_disp = lvgl_port_add_disp_dsi(&disp, &dsi);
    return s_disp ? ESP_OK : ESP_FAIL;
}

static esp_err_t touch_init(void) {
    const i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c), TAG, "I2C");

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = PIN_TOUCH_RST,
        .int_gpio_num = PIN_TOUCH_INT,
        .levels = {.reset = 0, .interrupt = 0},
    };
    // The GT911 answers on 0x5D or 0x14 depending on the INT level at reset: try both.
    const uint32_t addresses[] = {ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP};
    esp_err_t err = ESP_FAIL;
    for (size_t i = 0; i < 2 && err != ESP_OK; i++) {
        esp_lcd_panel_io_handle_t io = NULL;
        io_cfg.dev_addr = addresses[i];
        err = esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io);
        if (err == ESP_OK) err = esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &s_touch);
        if (err != ESP_OK && io) esp_lcd_panel_io_del(io);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "GT911 not found at 0x5D or 0x14");

    const lvgl_port_touch_cfg_t touch = {.disp = s_disp, .handle = s_touch};
    s_indev = lvgl_port_add_touch(&touch);
    return s_indev ? ESP_OK : ESP_FAIL;
}

esp_err_t board_init(void) {
    ESP_RETURN_ON_ERROR(power_init(), TAG, "power");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel");
    ESP_RETURN_ON_ERROR(lvgl_init(), TAG, "lvgl");
    esp_err_t err = touch_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "touch init failed (%s); continuing without touch", esp_err_to_name(err));
    ESP_LOGI(TAG, "display %dx%d ready%s", BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_indev ? ", touch ready" : "");
    return ESP_OK;
}

lv_display_t *board_display(void) { return s_disp; }
lv_indev_t *board_touch(void) { return s_indev; }
