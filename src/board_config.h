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

constexpr int LCD_WIDTH  = 320;
constexpr int LCD_HEIGHT = 240;
constexpr uint32_t LCD_SPI_FREQ_HZ = 27 * 1000 * 1000;
constexpr int LCD_ROTATION = 3;

// Physical ILI9341 panel memory geometry stays portrait even when runtime UI
// is landscape. Tune offsets if any edge is clipped.
constexpr int LCD_PANEL_MEMORY_WIDTH  = 240;
constexpr int LCD_PANEL_MEMORY_HEIGHT = 320;
constexpr int LCD_PANEL_OFFSET_X = 0;
constexpr int LCD_PANEL_OFFSET_Y = 0;

// Touch SPI bus (HSPI)
constexpr int TOUCH_SCLK = 25;
constexpr int TOUCH_MOSI = 32;
constexpr int TOUCH_MISO = 39;
constexpr int TOUCH_CS_PIN = 33;
constexpr int TOUCH_IRQ  = -1;

}  // namespace board
