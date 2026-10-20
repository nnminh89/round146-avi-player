#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// MJPEG frames -> screen. Frames are decoded on the caller's task (the AVI
// player) into one of two PSRAM frame buffers while a display task sends the
// other one, so decoding and the QSPI transfer overlap.
namespace video {

esp_err_t init();

// Call before each file starts: picks the decoder size again and clears the
// screen on the first frame.
void reset();

// One JPEG frame of a `width` x `height` video. Blocks while both frame
// buffers are still busy. Frames larger than the screen are scaled down to
// fit (in steps of 8 pixels, a decoder limit); smaller ones are centred.
void onFrame(const uint8_t* jpeg, size_t len, int width, int height);

// Frames shown since reset(); 0 at the end of a file means it did not play.
uint32_t framesShown();

}  // namespace video
