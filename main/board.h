#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Waveshare ESP32-S3-Touch-LCD-1.46(B): 412x412 round SPD2010 panel on QSPI,
// SPD2010 touch on I2C, TCA9554 expander for the resets and the SD card's D3.
namespace board {

constexpr int kWidth = 412;
constexpr int kHeight = 412;

// Power latch, I2C, expander, display (black), backlight, touch.
esp_err_t init();

// Called on each band of pixels on its way to the panel (RGB565 big-endian,
// `w` wide, top-left at screen x0, y0), e.g. to blend a menu over a video.
using Overlay = void (*)(uint8_t* band, int x0, int y0, int w, int rows);

// Draws a w x h RGB565 (big-endian) image whose rows are `stride` pixels
// apart, with its top-left corner at (x, y). The SPD2010 only accepts
// columns in groups of 4: x is rounded down and w cut down to a multiple of 4.
// Copies through internal DMA buffers, so `src` may live in PSRAM.
void draw(int x, int y, int w, int h, const uint8_t* src, int stride, Overlay overlay = nullptr);

// Fills the whole screen with one RGB565 colour (native, e.g. 0xF800 = red).
void fill(uint16_t rgb565);

// Backlight in percent.
void setBacklight(int percent);

// True while a finger is on the screen, with its position.
bool touched(int* x = nullptr, int* y = nullptr);

// True while the PWR button is held.
bool powerButtonDown();

// Screen and backlight off, then releases the power latch: on battery the
// board switches off. On USB power it cannot, so it stays dark until PWR is
// pressed again, then restarts. Does not return.
[[noreturn]] void powerOff();

}  // namespace board
