#include "video.h"

#include <algorithm>
#include <atomic>

#include "board.h"
#include "ui.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace video {
namespace {

constexpr const char* kTag = "video";
constexpr size_t kFrameBytes = board::kWidth * board::kHeight * 2;

struct Frame {
  int index;  // which frame buffer
  int width;
  int height;
  bool clear;  // first frame of a file: paint the screen black first
};

uint8_t* g_buffers[2] = {};
QueueHandle_t g_free = nullptr;   // int: buffers ready to be decoded into
QueueHandle_t g_ready = nullptr;  // Frame: decoded, waiting for the screen

jpeg_dec_handle_t g_decoder = nullptr;
int g_source_w = 0;  // video size the decoder is set up for
int g_source_h = 0;
int g_out_w = 0;
int g_out_h = 0;
bool g_scaled = false;
bool g_first = true;
bool g_warned = false;
std::atomic<uint32_t> g_shown{0};
// Decode time over the stats window, reported with the frame rate.
std::atomic<uint32_t> g_decode_us{0};
std::atomic<uint32_t> g_decoded{0};

// Largest size with the video's aspect ratio that fits the screen, in steps
// of 8 (decoder limit for scaling).
void fitScaled(int w, int h, int* out_w, int* out_h) {
  const float s = std::min(static_cast<float>(board::kWidth) / w, static_cast<float>(board::kHeight) / h);
  *out_w = std::max(8, static_cast<int>(w * s) / 8 * 8);
  *out_h = std::max(8, static_cast<int>(h * s) / 8 * 8);
}

bool openDecoder(int w, int h) {
  if (g_decoder) {
    jpeg_dec_close(g_decoder);
    g_decoder = nullptr;
  }
  jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
  cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;  // the panel's byte order
  g_out_w = w;
  g_out_h = h;
  g_scaled = w > board::kWidth || h > board::kHeight;
  if (g_scaled) {
    fitScaled(w, h, &g_out_w, &g_out_h);
    cfg.scale.width = static_cast<uint16_t>(g_out_w);
    cfg.scale.height = static_cast<uint16_t>(g_out_h);
    ESP_LOGW(kTag, "%dx%d video scaled to %dx%d; encode at <= 412x412 for best speed", w, h, g_out_w, g_out_h);
  }
  if (jpeg_dec_open(&cfg, &g_decoder) != JPEG_ERR_OK) {
    ESP_LOGE(kTag, "cannot open the JPEG decoder");
    g_decoder = nullptr;
    return false;
  }
  g_source_w = w;
  g_source_h = h;
  return true;
}

// Whether the video rectangle covers the whole menu.
bool coversMenu(int x, int y, int w, int h) {
  int mx = 0, my = 0, mw = 0, mh = 0;
  ui::bounds(&mx, &my, &mw, &mh);
  return x <= mx && y <= my && x + w >= mx + mw && y + h >= my + mh;
}

void displayTask(void*) {
  int64_t window_start = esp_timer_get_time();
  uint32_t window_frames = 0;
  bool menu_was_visible = false;
  while (true) {
    Frame f;
    xQueueReceive(g_ready, &f, portMAX_DELAY);
    const int x = (board::kWidth - f.width) / 2;
    const int y = (board::kHeight - f.height) / 2;
    const bool covers = coversMenu(x, y, f.width, f.height);
    const bool menu = ui::visible();
    const bool menu_changed = ui::refresh();
    // A video smaller than the menu leaves parts of it on black: draw those
    // when the menu changes, wipe them when it closes.
    if (f.clear || (!menu && menu_was_visible && !covers)) {
      board::fill(0x0000);
    } else if (menu && menu_changed && !covers) {
      ui::drawOnBlack();
    }
    menu_was_visible = menu;
    board::draw(x, y, f.width, f.height, g_buffers[f.index], f.width, menu ? &ui::compose : nullptr);
    xQueueSend(g_free, &f.index, portMAX_DELAY);
    g_shown.fetch_add(1);

    ++window_frames;
    const int64_t now = esp_timer_get_time();
    if (now - window_start >= 5'000'000) {
      const uint32_t decoded = g_decoded.exchange(0);
      const uint32_t decode_us = g_decode_us.exchange(0);
      ESP_LOGI(kTag, "%.1f fps shown, %dx%d, decode %.1f ms/frame",
               window_frames * 1e6 / static_cast<double>(now - window_start), f.width, f.height,
               decoded ? decode_us / 1000.0 / decoded : 0.0);
      window_start = now;
      window_frames = 0;
    }
  }
}

}  // namespace

esp_err_t init() {
  g_free = xQueueCreate(2, sizeof(int));
  g_ready = xQueueCreate(2, sizeof(Frame));
  ESP_RETURN_ON_FALSE(g_free && g_ready, ESP_ERR_NO_MEM, kTag, "queues");
  for (int i = 0; i < 2; ++i) {
    // The decoder wants 16-byte aligned output.
    g_buffers[i] = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, kFrameBytes, MALLOC_CAP_SPIRAM));
    ESP_RETURN_ON_FALSE(g_buffers[i], ESP_ERR_NO_MEM, kTag, "frame buffer");
    xQueueSend(g_free, &i, 0);
  }
  ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(&displayTask, "display", 4096, nullptr, 6, nullptr, 0) == pdPASS,
                      ESP_ERR_NO_MEM, kTag, "display task");
  return ESP_OK;
}

void reset() {
  g_first = true;
  g_warned = false;
  g_shown.store(0);
}

void onFrame(const uint8_t* jpeg, size_t len, int width, int height) {
  // Encoders write empty (or tiny) chunks for "same picture as before":
  // keep showing the last frame.
  if (len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
    return;
  }
  if (!g_decoder || width != g_source_w || height != g_source_h) {
    if (!openDecoder(width, height)) {
      return;
    }
  }
  int index = 0;
  xQueueReceive(g_free, &index, portMAX_DELAY);

  jpeg_dec_io_t io = {};
  io.inbuf = const_cast<uint8_t*>(jpeg);
  io.inbuf_len = static_cast<int>(len);
  jpeg_dec_header_info_t info = {};
  int out_len = 0;
  jpeg_error_t err = jpeg_dec_parse_header(g_decoder, &io, &info);
  if (err == JPEG_ERR_OK) {
    err = jpeg_dec_get_outbuf_len(g_decoder, &out_len);
  }
  if (err == JPEG_ERR_OK && (out_len <= 0 || static_cast<size_t>(out_len) > kFrameBytes)) {
    err = JPEG_ERR_NO_MEM;
  }
  if (err == JPEG_ERR_OK) {
    io.outbuf = g_buffers[index];
    const int64_t t0 = esp_timer_get_time();
    err = jpeg_dec_process(g_decoder, &io);
    g_decode_us.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - t0));
    g_decoded.fetch_add(1);
  }
  if (err != JPEG_ERR_OK) {
    if (!g_warned) {
      ESP_LOGE(kTag, "JPEG frame not decoded (error %d): re-encode as baseline MJPEG", static_cast<int>(err));
      g_warned = true;
    }
    xQueueSend(g_free, &index, portMAX_DELAY);
    return;
  }

  // Unscaled frames take their size from the JPEG itself, in case the AVI
  // header says otherwise.
  Frame f = {index, g_scaled ? g_out_w : std::min<int>(info.width, board::kWidth),
             g_scaled ? g_out_h : std::min<int>(info.height, board::kHeight), g_first};
  g_first = false;
  xQueueSend(g_ready, &f, portMAX_DELAY);
}

uint32_t framesShown() {
  return g_shown.load();
}

}  // namespace video
