#include "audio.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

namespace audio {
namespace {

constexpr const char* kTag = "audio";
constexpr gpio_num_t kBclk = GPIO_NUM_48;
constexpr gpio_num_t kLrck = GPIO_NUM_38;
constexpr gpio_num_t kDout = GPIO_NUM_47;

i2s_chan_handle_t g_tx = nullptr;
bool g_enabled = false;
bool g_active = false;  // the current file has playable sound

}  // namespace

esp_err_t init() {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.auto_clear = true;  // send silence when starved instead of repeating
  chan.dma_desc_num = 6;
  chan.dma_frame_num = 480;
  ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &g_tx, nullptr), kTag, "I2S channel");

  i2s_std_config_t std = {};
  std.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(22050);
  std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  std.gpio_cfg.mclk = I2S_GPIO_UNUSED;  // the PCM5101 makes its own clock
  std.gpio_cfg.bclk = kBclk;
  std.gpio_cfg.ws = kLrck;
  std.gpio_cfg.dout = kDout;
  std.gpio_cfg.din = I2S_GPIO_UNUSED;
  return i2s_channel_init_std_mode(g_tx, &std);
}

void configure(uint32_t sample_rate, uint32_t bits, uint32_t channels) {
  stop();
  g_active = false;
  if (sample_rate == 0 || channels == 0) {
    ESP_LOGI(kTag, "no soundtrack");
    return;
  }
  if (bits != 16 || channels > 2) {
    ESP_LOGW(kTag, "soundtrack %lu-bit %lu ch not supported (use 16-bit PCM): muted", static_cast<unsigned long>(bits),
             static_cast<unsigned long>(channels));
    return;
  }
  i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
  i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO);
  if (i2s_channel_reconfig_std_clock(g_tx, &clk) != ESP_OK || i2s_channel_reconfig_std_slot(g_tx, &slot) != ESP_OK ||
      i2s_channel_enable(g_tx) != ESP_OK) {
    ESP_LOGE(kTag, "cannot start I2S at %lu Hz", static_cast<unsigned long>(sample_rate));
    return;
  }
  g_enabled = true;
  g_active = true;
  ESP_LOGI(kTag, "sound %lu Hz, %s", static_cast<unsigned long>(sample_rate), channels == 1 ? "mono" : "stereo");
}

void write(uint8_t* data, size_t len, int volume, bool muted) {
  if (!g_active) {
    return;
  }
  auto* samples = reinterpret_cast<int16_t*>(data);
  const size_t count = len / 2;
  // Muted sound is still written (as silence) so the timing stays the same.
  // The slider is squared: it feels even to the ear.
  const int gain = muted ? 0 : volume * volume;  // 0..10000
  if (gain < 10000) {
    for (size_t i = 0; i < count; ++i) {
      samples[i] = static_cast<int16_t>(samples[i] * gain / 10000);
    }
  }
  size_t written = 0;
  i2s_channel_write(g_tx, data, count * 2, &written, pdMS_TO_TICKS(500));
}

void stop() {
  if (g_enabled) {
    i2s_channel_disable(g_tx);
    g_enabled = false;
  }
}

}  // namespace audio
