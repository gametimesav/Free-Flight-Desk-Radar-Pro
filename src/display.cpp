#include "display.h"
#include "board_config.h"

#include <TFT_eSPI.h>
#include <lvgl.h>

namespace display {
namespace {

TFT_eSPI tft = TFT_eSPI();

// Slightly larger band while staying within ESP32 DRAM limits.
constexpr size_t BUF_LINES = 16;
constexpr size_t BUF_PIXELS = static_cast<size_t>(board::LCD_WIDTH) * BUF_LINES;
constexpr size_t BUF_BYTES  = BUF_PIXELS * sizeof(lv_color_t);

DMA_ATTR uint8_t buf_a[BUF_BYTES];

lv_display_t* lv_disp = nullptr;
uint8_t       bl_value = 255;
uint32_t      last_tick_ms = 0;

// LVGL flush callback — push rendered region to the panel.
void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const uint32_t w = area->x2 - area->x1 + 1;
    const uint32_t h = area->y2 - area->y1 + 1;

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushPixels(reinterpret_cast<const uint16_t*>(px_map), w * h);
    tft.endWrite();
    lv_display_flush_ready(disp);
}

}  // namespace

void begin() {
    // Force BL pin to a known state before panel init; some boards boot with
    // the backlight transistor off until the pin is explicitly driven.
    pinMode(board::LCD_BL, OUTPUT);
    digitalWrite(board::LCD_BL, HIGH);

    // Display init
    tft.begin();
    tft.setRotation(board::LCD_ROTATION);
    tft.setSwapBytes(true);
    tft.fillScreen(TFT_BLACK);

    // LVGL init
    lv_init();

    lv_disp = lv_display_create(board::LCD_WIDTH, board::LCD_HEIGHT);
    lv_display_set_flush_cb(lv_disp, flush_cb);
    lv_display_set_buffers(lv_disp, buf_a, nullptr, BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(lv_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_antialiasing(lv_disp, true);

    last_tick_ms = millis();
    set_brightness(bl_value);
}

void tick() {
    const uint32_t now = millis();
    const uint32_t elapsed = now - last_tick_ms;
    if (elapsed > 0) {
        lv_tick_inc(elapsed);
        last_tick_ms = now;
    }
    lv_timer_handler();
}

void set_brightness(uint8_t value) {
    bl_value = value;
    digitalWrite(board::LCD_BL, value > 0 ? HIGH : LOW);
}

uint8_t brightness() { return bl_value; }

}  // namespace display
