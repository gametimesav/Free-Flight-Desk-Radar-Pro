// Hardware pin map for ESP32 Dev + ILI9341 + XPT2046 touch.
#pragma once

#include <Arduino.h>

namespace board {

// Display SPI bus (VSPI)
constexpr int LCD_SCLK = 14;
constexpr int LCD_MOSI = 13;
constexpr int LCD_MISO = 12;
constexpr int LCD_DC   = 2;
constexpr int LCD_CS   = 15;
constexpr int LCD_RST  = -1;
constexpr int LCD_BL   = 21;

// This panel is a 320x240 landscape display. Keep the runtime UI and TFT
// geometry aligned to the physical panel instead of the previous portrait
// 240x320 assumption.
constexpr int LCD_WIDTH  = 320;
constexpr int LCD_HEIGHT = 240;
constexpr uint32_t LCD_SPI_FREQ_HZ = 16 * 1000 * 1000;
constexpr int LCD_ROTATION = 1;

// Physical ILI9341 panel memory geometry remains landscape for the runtime UI.
constexpr int LCD_PANEL_MEMORY_WIDTH  = 320;
constexpr int LCD_PANEL_MEMORY_HEIGHT = 240;
constexpr int LCD_PANEL_OFFSET_X = 0;
constexpr int LCD_PANEL_OFFSET_Y = 0;

// Touch SPI bus (HSPI)
constexpr int TOUCH_SCLK = 25;
constexpr int TOUCH_MOSI = 32;
constexpr int TOUCH_MISO = 39;
constexpr int TOUCH_CS_PIN = 33;
constexpr int TOUCH_IRQ  = -1;

}  // namespace board
