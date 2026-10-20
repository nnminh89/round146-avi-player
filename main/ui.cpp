#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "icons.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace ui {
namespace {

constexpr const char* kTag = "ui";
constexpr const char* kNvsNamespace = "player";

// Layout (screen pixels, screen centre is 206, 206).
constexpr int kButtonR = 46;
constexpr int kButtonY = 172;
constexpr int kButtonX[3] = {206 - 112, 206, 206 + 112};
enum Button { kMute = 0, kNext = 1, kMode = 2 };
constexpr int kSliderY = 270;
constexpr int kSliderX1 = 206 - 118;
constexpr int kSliderX2 = 206 + 118;
constexpr int kKnobR = 14;
// Menu image: covers the buttons and the slider. x and w are multiples of 4
// (panel column groups).
constexpr int kBoxX = 40;
constexpr int kBoxY = 118;
constexpr int kBoxW = 332;
constexpr int kBoxH = 184;

constexpr int64_t kHideAfterUs = 4'000'000;
constexpr int64_t kFlashUs = 180'000;  // a pressed button lights up this long

// Colours (RGB565).
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kDark = 0x10A3;
constexpr uint16_t kPressed = 0x4A69;
constexpr uint16_t kMutedRed = 0xC145;

struct State {
  bool visible = false;
  Settings s = {false, Mode::LoopOne, CONFIG_PLAYER_VOLUME};
  int pressed = -1;  // button lit after a tap
};

SemaphoreHandle_t g_lock = nullptr;
State g_state;
uint32_t g_version = 1;  // bumped on every visible change

// Touch-task-only bookkeeping.
bool g_was_down = false;
bool g_dragging = false;
bool g_swallow = false;  // the press that opened the menu does nothing else
int64_t g_last_activity = 0;
int64_t g_flash_until = 0;
bool g_volume_dirty = false;

// Display-task-only: the rendered menu.
uint16_t* g_color = nullptr;  // RGB565, native order
uint8_t* g_alpha = nullptr;
uint8_t* g_black = nullptr;   // menu over black, big-endian, for drawOnBlack()
uint32_t g_drawn_version = 0;

class Lock {
 public:
  Lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(g_lock); }
};

void save(const Settings& s) {
  nvs_handle_t h = 0;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
    return;
  }
  nvs_set_u8(h, "muted", s.muted ? 1 : 0);
  nvs_set_u8(h, "mode", static_cast<uint8_t>(s.mode));
  nvs_set_u8(h, "volume", s.volume);
  nvs_commit(h);
  nvs_close(h);
}

// --- Drawing into the menu image ------------------------------------------

// "Over" blend of a colour with coverage `a` (0..1) into the image.
void plot(int x, int y, uint16_t c, float a) {
  if (a <= 0.0f || x < 0 || y < 0 || x >= kBoxW || y >= kBoxH) {
    return;
  }
  const int i = y * kBoxW + x;
  const float da = g_alpha[i] / 255.0f;
  const float oa = a + da * (1.0f - a);
  if (oa <= 0.0f) {
    return;
  }
  const uint16_t d = g_color[i];
  const auto mix = [&](int sc, int dc) {
    return static_cast<int>(std::lround((sc * a + dc * da * (1.0f - a)) / oa));
  };
  const int r = mix((c >> 11) & 31, (d >> 11) & 31);
  const int g = mix((c >> 5) & 63, (d >> 5) & 63);
  const int b = mix(c & 31, d & 31);
  g_color[i] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
  g_alpha[i] = static_cast<uint8_t>(std::lround(oa * 255.0f));
}

// Anti-aliased capsule: every point within `r` of the segment (x1,y1)-(x2,y2),
// in screen coordinates. A disc when both ends are the same.
void capsule(float x1, float y1, float x2, float y2, float r, uint16_t c, float alpha) {
  const int bx0 = static_cast<int>(std::floor(std::min(x1, x2) - r - 1)) - kBoxX;
  const int bx1 = static_cast<int>(std::ceil(std::max(x1, x2) + r + 1)) - kBoxX;
  const int by0 = static_cast<int>(std::floor(std::min(y1, y2) - r - 1)) - kBoxY;
  const int by1 = static_cast<int>(std::ceil(std::max(y1, y2) + r + 1)) - kBoxY;
  const float dx = x2 - x1;
  const float dy = y2 - y1;
  const float len2 = dx * dx + dy * dy;
  for (int y = std::max(0, by0); y <= std::min(kBoxH - 1, by1); ++y) {
    for (int x = std::max(0, bx0); x <= std::min(kBoxW - 1, bx1); ++x) {
      const float px = x + kBoxX + 0.5f;
      const float py = y + kBoxY + 0.5f;
      float t = len2 > 0 ? ((px - x1) * dx + (py - y1) * dy) / len2 : 0.0f;
      t = std::clamp(t, 0.0f, 1.0f);
      const float d = std::hypot(px - (x1 + t * dx), py - (y1 + t * dy));
      plot(x, y, c, alpha * std::clamp(r - d + 0.5f, 0.0f, 1.0f));
    }
  }
}

void icon(const uint8_t* mask, int cx, int cy) {
  const int x0 = cx - icons::kSize / 2 - kBoxX;
  const int y0 = cy - icons::kSize / 2 - kBoxY;
  for (int y = 0; y < icons::kSize; ++y) {
    for (int x = 0; x < icons::kSize; ++x) {
      plot(x0 + x, y0 + y, kWhite, mask[y * icons::kSize + x] / 255.0f);
    }
  }
}

float sliderX(int volume) {
  return kSliderX1 + (kSliderX2 - kSliderX1) * volume / 100.0f;
}

void render(const State& st) {
  memset(g_alpha, 0, kBoxW * kBoxH);
  memset(g_color, 0, kBoxW * kBoxH * sizeof(uint16_t));

  for (int b = 0; b < 3; ++b) {
    uint16_t fill = kDark;
    float alpha = 0.68f;
    if (b == kMute && st.s.muted) {
      fill = kMutedRed;
      alpha = 0.85f;
    }
    if (b == st.pressed) {
      fill = kPressed;
      alpha = 0.92f;
    }
    capsule(kButtonX[b], kButtonY, kButtonX[b], kButtonY, kButtonR, fill, alpha);
  }
  icon(st.s.muted ? icons::kSoundOff : icons::kSoundOn, kButtonX[kMute], kButtonY);
  icon(icons::kNext, kButtonX[kNext], kButtonY);
  icon(st.s.mode == Mode::LoopOne ? icons::kRepeatOne : icons::kRepeatAll, kButtonX[kMode], kButtonY);

  // Slider: dark pill behind, faint track, bright part up to the knob.
  const float knob = sliderX(st.s.volume);
  capsule(kSliderX1, kSliderY, kSliderX2, kSliderY, kKnobR + 9, kDark, 0.68f);
  capsule(kSliderX1, kSliderY, kSliderX2, kSliderY, 4, kWhite, 0.3f);
  if (st.s.volume > 0) {
    capsule(kSliderX1, kSliderY, knob, kSliderY, 4, kWhite, 1.0f);
  }
  capsule(knob, kSliderY, knob, kSliderY, kKnobR, kWhite, 1.0f);

  // The same image over black, ready to send.
  for (int i = 0; i < kBoxW * kBoxH; ++i) {
    const uint16_t c = g_color[i];
    const int a = g_alpha[i];
    const int r = ((c >> 11) & 31) * a / 255;
    const int g = ((c >> 5) & 63) * a / 255;
    const int b = (c & 31) * a / 255;
    const uint16_t o = static_cast<uint16_t>((r << 11) | (g << 5) | b);
    g_black[i * 2] = o >> 8;
    g_black[i * 2 + 1] = o & 0xFF;
  }
}

// --- Touch -------------------------------------------------------------------

int hitButton(int x, int y) {
  for (int b = 0; b < 3; ++b) {
    const int dx = x - kButtonX[b];
    const int dy = y - kButtonY;
    if (dx * dx + dy * dy <= (kButtonR + 8) * (kButtonR + 8)) {
      return b;
    }
  }
  return -1;
}

bool onSlider(int x, int y) {
  return std::abs(y - kSliderY) <= 32 && x >= kSliderX1 - 24 && x <= kSliderX2 + 24;
}

int volumeAt(int x) {
  return std::clamp(static_cast<int>(std::lround((x - kSliderX1) * 100.0f / (kSliderX2 - kSliderX1))), 0, 100);
}

}  // namespace

esp_err_t init() {
  g_lock = xSemaphoreCreateMutex();
  g_color = static_cast<uint16_t*>(heap_caps_malloc(kBoxW * kBoxH * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
  g_alpha = static_cast<uint8_t*>(heap_caps_malloc(kBoxW * kBoxH, MALLOC_CAP_SPIRAM));
  g_black = static_cast<uint8_t*>(heap_caps_malloc(kBoxW * kBoxH * 2, MALLOC_CAP_SPIRAM));
  if (!g_lock || !g_color || !g_alpha || !g_black) {
    return ESP_ERR_NO_MEM;
  }
  nvs_handle_t h = 0;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) == ESP_OK) {
    uint8_t v = 0;
    if (nvs_get_u8(h, "muted", &v) == ESP_OK) g_state.s.muted = v != 0;
    if (nvs_get_u8(h, "mode", &v) == ESP_OK) g_state.s.mode = v == 1 ? Mode::PlayAll : Mode::LoopOne;
    if (nvs_get_u8(h, "volume", &v) == ESP_OK) g_state.s.volume = std::min<uint8_t>(v, 100);
    nvs_close(h);
  }
  ESP_LOGI(kTag, "sound %s, volume %d%%, %s", g_state.s.muted ? "muted" : "on", g_state.s.volume,
           g_state.s.mode == Mode::LoopOne ? "loop one video" : "play all videos");
  return ESP_OK;
}

void setMutedNow() {
  Lock lock;
  g_state.s.muted = true;
}

Settings settings() {
  Lock lock;
  return g_state.s;
}

bool visible() {
  Lock lock;
  return g_state.visible;
}

Action onTouch(bool down, int x, int y) {
  const int64_t now = esp_timer_get_time();
  Action action = Action::None;
  Settings to_save = {};
  bool save_now = false;
  {
    Lock lock;
    State& st = g_state;
    const auto changed = [&] { ++g_version; };

    if (down && !g_was_down) {  // a new press
      g_last_activity = now;
      if (!st.visible) {
        st.visible = true;
        g_swallow = true;
        changed();
        ESP_LOGI(kTag, "menu open (tap at %d,%d)", x, y);
      } else if (onSlider(x, y)) {
        g_dragging = true;
      } else if (const int b = hitButton(x, y); b >= 0) {
        st.pressed = b;
        g_flash_until = now + kFlashUs;
        if (b == kMute) {
          st.s.muted = !st.s.muted;
          save_now = true;
        } else if (b == kMode) {
          st.s.mode = st.s.mode == Mode::LoopOne ? Mode::PlayAll : Mode::LoopOne;
          save_now = true;
        } else {
          action = Action::Next;
        }
        ESP_LOGI(kTag, "button %d at %d,%d", b, x, y);
        changed();
      } else {
        st.visible = false;
        changed();
        ESP_LOGI(kTag, "menu closed (tap at %d,%d)", x, y);
      }
    }
    if (down && g_dragging && !g_swallow) {
      g_last_activity = now;
      const int v = volumeAt(x);
      if (v != st.s.volume) {
        st.s.volume = static_cast<uint8_t>(v);
        g_volume_dirty = true;
        changed();
      }
    }
    if (!down && g_was_down) {  // released
      g_dragging = false;
      g_swallow = false;
      if (g_volume_dirty) {
        g_volume_dirty = false;
        save_now = true;
        ESP_LOGI(kTag, "volume %d%%", st.s.volume);
      }
    }
    if (st.pressed >= 0 && now > g_flash_until) {
      st.pressed = -1;
      changed();
    }
    if (st.visible && !down && now - g_last_activity > kHideAfterUs) {
      st.visible = false;
      changed();
    }
    g_was_down = down;
    to_save = st.s;
  }
  if (save_now) {
    save(to_save);
  }
  return action;
}

bool refresh() {
  State st;
  uint32_t version = 0;
  {
    Lock lock;
    st = g_state;
    version = g_version;
  }
  if (version == g_drawn_version) {
    return false;
  }
  g_drawn_version = version;
  if (st.visible) {
    render(st);
  }
  return true;
}

void compose(uint8_t* band, int x0, int y0, int w, int rows) {
  const int ys = std::max(y0, kBoxY);
  const int ye = std::min(y0 + rows, kBoxY + kBoxH);
  const int xs = std::max(x0, kBoxX);
  const int xe = std::min(x0 + w, kBoxX + kBoxW);
  for (int y = ys; y < ye; ++y) {
    const uint16_t* src = g_color + (y - kBoxY) * kBoxW;
    const uint8_t* alpha = g_alpha + (y - kBoxY) * kBoxW;
    uint8_t* dst = band + ((y - y0) * w) * 2;
    for (int x = xs; x < xe; ++x) {
      const int a = alpha[x - kBoxX];
      if (a == 0) {
        continue;
      }
      uint8_t* p = dst + (x - x0) * 2;
      const uint16_t c = src[x - kBoxX];
      const uint16_t d = static_cast<uint16_t>((p[0] << 8) | p[1]);
      const int na = 255 - a;
      const int r = (((c >> 11) & 31) * a + ((d >> 11) & 31) * na) / 255;
      const int g = (((c >> 5) & 63) * a + ((d >> 5) & 63) * na) / 255;
      const int b = ((c & 31) * a + (d & 31) * na) / 255;
      const uint16_t o = static_cast<uint16_t>((r << 11) | (g << 5) | b);
      p[0] = o >> 8;
      p[1] = o & 0xFF;
    }
  }
}

void bounds(int* x, int* y, int* w, int* h) {
  *x = kBoxX;
  *y = kBoxY;
  *w = kBoxW;
  *h = kBoxH;
}

void drawOnBlack() {
  board::draw(kBoxX, kBoxY, kBoxW, kBoxH, g_black, kBoxW);
}

void showNotFound() {
  static uint8_t* text = nullptr;  // white text on black, big-endian RGB565
  if (!text) {
    text = static_cast<uint8_t*>(heap_caps_malloc(icons::kMessageW * icons::kMessageH * 2, MALLOC_CAP_SPIRAM));
    if (!text) {
      board::fill(0x0000);
      return;
    }
    for (int i = 0; i < icons::kMessageW * icons::kMessageH; ++i) {
      const int a = icons::kMessage[i];
      const uint16_t c = static_cast<uint16_t>(((a * 31 / 255) << 11) | ((a * 63 / 255) << 5) | (a * 31 / 255));
      text[i * 2] = c >> 8;
      text[i * 2 + 1] = c & 0xFF;
    }
  }
  board::fill(0x0000);
  board::draw((board::kWidth - icons::kMessageW) / 2, (board::kHeight - icons::kMessageH) / 2, icons::kMessageW,
              icons::kMessageH, text, icons::kMessageW);
}

}  // namespace ui
