#include "display.h"
#include "board_config.h"

#include <LovyanGFX.hpp>   // LGFX_USE_V1 is set via build_flags
#include <lvgl.h>

namespace display {
namespace {

// LovyanGFX panel definition for ESP32 + ILI9341.
class LGFX_ILI9341 : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 _panel;
    lgfx::Bus_SPI      _bus;
    lgfx::Light_PWM    _light;
public:
    LGFX_ILI9341() {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = board::LCD_SPI_FREQ_HZ;
            cfg.freq_read   = 16'000'000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = board::LCD_SCLK;
            cfg.pin_mosi    = board::LCD_MOSI;
            cfg.pin_miso    = board::LCD_MISO;
            cfg.pin_dc      = board::LCD_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs           = board::LCD_CS;
            cfg.pin_rst          = board::LCD_RST;
            cfg.pin_busy         = -1;
            cfg.memory_width     = board::LCD_PANEL_MEMORY_WIDTH;
            cfg.memory_height    = board::LCD_PANEL_MEMORY_HEIGHT;
            cfg.panel_width      = board::LCD_PANEL_MEMORY_WIDTH;
            cfg.panel_height     = board::LCD_PANEL_MEMORY_HEIGHT;
            cfg.offset_x         = board::LCD_PANEL_OFFSET_X;
            cfg.offset_y         = board::LCD_PANEL_OFFSET_Y;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            cfg.invert           = false;
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = board::LCD_BL;
            cfg.invert      = false;
            cfg.freq        = 12000;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

LGFX_ILI9341 tft;

// LVGL draw buffer kept intentionally small for non-PSRAM ESP32 targets.
constexpr size_t BUF_LINES = 10;
constexpr size_t BUF_PIXELS = static_cast<size_t>(board::LCD_WIDTH) * BUF_LINES;
constexpr size_t BUF_BYTES  = BUF_PIXELS * sizeof(lv_color_t);

DMA_ATTR uint8_t buf_a[BUF_BYTES];

lv_display_t* lv_disp = nullptr;
uint8_t       bl_value = 255;
uint32_t      last_tick_ms = 0;

// LVGL flush callback — push rendered region to the panel via DMA.
// Signalling flush_ready immediately is safe with exactly two LVGL buffers:
// LVGL renders the next band into the *other* buffer, and the next
// pushImageDMA call waits for this transfer to finish before reusing the bus,
// so a buffer is never overwritten while DMA is still reading it.
void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const uint32_t w = area->x2 - area->x1 + 1;
    const uint32_t h = area->y2 - area->y1 + 1;
    // LVGL draws RGB565 little-endian; swap to the panel byte order.
    lv_draw_sw_rgb565_swap(px_map, w * h);

    // Keep flush synchronous here: this target uses a single LVGL buffer, and
    // returning before transfer completion can cause the same buffer to be
    // reused while data is still in-flight.
    tft.pushImage(area->x1, area->y1, w, h,
                  reinterpret_cast<const lgfx::swap565_t*>(px_map));
    lv_display_flush_ready(disp);
}

}  // namespace

void begin() {
    // Force BL pin to a known state before panel init; some boards boot with
    // the backlight transistor off until the pin is explicitly driven.
    pinMode(board::LCD_BL, OUTPUT);
    digitalWrite(board::LCD_BL, HIGH);

    // Display init
    tft.init();
    tft.setRotation(board::LCD_ROTATION);
    tft.setBrightness(255);

    // Quick visible test pattern to confirm panel + backlight are active.
    tft.fillScreen(TFT_RED);
    delay(80);
    tft.fillScreen(TFT_GREEN);
    delay(80);
    tft.fillScreen(TFT_BLUE);
    delay(80);
    tft.fillScreen(TFT_BLACK);

    // LVGL init
    lv_init();

    lv_disp = lv_display_create(board::LCD_WIDTH, board::LCD_HEIGHT);
    lv_display_set_flush_cb(lv_disp, flush_cb);
    lv_display_set_buffers(lv_disp, buf_a, nullptr, BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(lv_disp, LV_COLOR_FORMAT_RGB565);

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
    tft.setBrightness(value);
}

uint8_t brightness() { return bl_value; }

}  // namespace display
