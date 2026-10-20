#pragma once

#include <cstdint>

#include "esp_err.h"

// The on-screen menu: a tap opens it over the playing video. Three round
// icon buttons (sound on/off, next video, loop one / play all) and a volume
// slider; no text. A tap outside them, or 4 s without touching, closes it.
// Sound, mode and volume are kept in NVS.
namespace ui {

enum class Mode : uint8_t {
  LoopOne,  // repeat the current video
  PlayAll,  // go on to the next video, back to the first after the last
};

struct Settings {
  bool muted;
  Mode mode;
  uint8_t volume;  // 0..100
};

esp_err_t init();  // loads the saved settings (NVS must be initialised)
// Silences the sound right away without saving it (switching off).
void setMutedNow();
Settings settings();

enum class Action : uint8_t { None, Next };

// Touch task, every poll: whether a finger is down and where. Opens and
// closes the menu, runs the buttons, drags the slider.
Action onTouch(bool down, int x, int y);

// Display task.
bool visible();
// Rebuilds the menu image if something changed since the last call; true if
// it did (the menu looks different now).
bool refresh();
// Blends the menu into a band of screen pixels (RGB565 big-endian, `w` wide,
// top-left at screen x0, y0). Matches board::Overlay.
void compose(uint8_t* band, int x0, int y0, int w, int rows);
// Screen rectangle the menu covers.
void bounds(int* x, int* y, int* w, int* h);
// Draws the menu over black (for videos that do not cover it).
void drawOnBlack();

// Black screen with "Video not found" in the middle (no card, no .avi file,
// or none of them plays).
void showNotFound();

}  // namespace ui
