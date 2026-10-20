#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// PCM soundtrack -> PCM5101 DAC (I2S) -> onboard speaker.
namespace audio {

esp_err_t init();

// Format of the next file's sound; 0 Hz or anything but 16-bit PCM mutes it.
void configure(uint32_t sample_rate, uint32_t bits, uint32_t channels);

// Writes PCM samples (blocks while the DMA queue is full, which also paces
// playback). Volume (0..100) and mute are applied in place.
void write(uint8_t* data, size_t len, int volume, bool muted);

// Silences the output between files.
void stop();

}  // namespace audio
