// Round AVI player: plays the .avi files on the microSD card on the 412x412
// round screen of a Waveshare ESP32-S3-Touch-LCD-1.46. A tap opens a menu
// (sound on/off, next video, loop one / play all, volume); each video loops,
// or the player goes through them all and back to the first.
// See README.md for the file format (MJPEG video, PCM sound).

#include <algorithm>
#include <atomic>
#include <cstring>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <vector>

#include "audio.h"
#include "avi_player.h"
#include "board.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"
#include "nvs_flash.h"
#include "ui.h"
#include "video.h"

namespace {

constexpr const char* kTag = "player";
constexpr const char* kMount = "/sdcard";
constexpr int kMaxDepth = 4;
// Largest AVI chunk (one JPEG frame or one block of sound).
constexpr size_t kChunkBytes = 512 * 1024;


enum class Event : uint8_t { Ended, Next };

QueueHandle_t g_events = nullptr;
avi_player_handle_t g_player = nullptr;
sdmmc_card_t* g_card = nullptr;
// Set when PWR switches the board off: nothing new starts playing.
std::atomic<bool> g_off{false};

// --- SD card -----------------------------------------------------------

bool mountOnce() {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;  // D1/D2 are not wired; D3 is held high by the expander
  slot.clk = GPIO_NUM_14;
  slot.cmd = GPIO_NUM_17;
  slot.d0 = GPIO_NUM_16;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
  esp_vfs_fat_sdmmc_mount_config_t mount = {};
  mount.format_if_mount_failed = false;
  mount.max_files = 4;
  mount.allocation_unit_size = 16 * 1024;
  const esp_err_t err = esp_vfs_fat_sdmmc_mount(kMount, &host, &slot, &mount, &g_card);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "no microSD card (%s)", esp_err_to_name(err));
    g_card = nullptr;
    return false;
  }
  sdmmc_card_print_info(stdout, g_card);
  return true;
}

// Right after power-up the card often misses the first attempt. `quiet`
// keeps the SD driver's errors out of the log (retries while no card is in).
bool mountCard(bool quiet = false) {
  if (g_card) {
    return true;
  }
  static const char* const kSdTags[] = {"sdmmc_common", "sdmmc_req", "vfs_fat_sdmmc", "SD_HOST", "SD_TRANS", kTag};
  for (const char* tag : kSdTags) {
    esp_log_level_set(tag, quiet ? ESP_LOG_NONE : ESP_LOG_INFO);
  }
  bool ok = false;
  for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
    ok = mountOnce();
    if (!ok) {
      vTaskDelay(pdMS_TO_TICKS(300));
    }
  }
  for (const char* tag : kSdTags) {
    esp_log_level_set(tag, ESP_LOG_INFO);
  }
  return ok;
}

void unmountCard() {
  if (g_card) {
    esp_vfs_fat_sdcard_unmount(kMount, g_card);
    g_card = nullptr;
  }
}

bool isAvi(const char* name) {
  const size_t len = strlen(name);
  return len > 4 && strcasecmp(name + len - 4, ".avi") == 0;
}

// Every .avi under `dir`, skipping hidden entries such as the "._name.avi"
// files macOS writes next to every file it copies to a FAT card.
void scan(const std::string& dir, int depth, std::vector<std::string>* out) {
  DIR* d = opendir(dir.c_str());
  if (!d) {
    return;
  }
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.' || strcasecmp(e->d_name, "System Volume Information") == 0) {
      continue;
    }
    const std::string path = dir + "/" + e->d_name;
    if (e->d_type == DT_DIR) {
      if (depth < kMaxDepth) {
        scan(path, depth + 1, out);
      }
    } else if (isAvi(e->d_name)) {
      out->push_back(path);
    }
  }
  closedir(d);
}

std::vector<std::string> findVideos() {
  std::vector<std::string> files;
  scan(kMount, 0, &files);
  std::sort(files.begin(), files.end(),
            [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
  ESP_LOGI(kTag, "%u video(s) on the card", static_cast<unsigned>(files.size()));
  for (size_t i = 0; i < files.size(); ++i) {
    ESP_LOGI(kTag, "  %u: %s", static_cast<unsigned>(i + 1), files[i].c_str() + strlen(kMount) + 1);
  }
  return files;
}

// --- AVI player callbacks (player task) ----------------------------------

void onVideo(frame_data_t* data, void*) {
  if (data->video_info.frame_format != FORMAT_MJEPG) {
    return;  // H.264 has no decoder here
  }
  video::onFrame(data->data, data->data_bytes, static_cast<int>(data->video_info.width),
                 static_cast<int>(data->video_info.height));
}

void onAudio(frame_data_t* data, void*) {
  const ui::Settings s = ui::settings();
  audio::write(data->data, data->data_bytes, s.volume, s.muted);
}

void onAudioFormat(uint32_t rate, uint32_t bits, uint32_t channels, void*) {
  audio::configure(rate, bits, channels);
}

void onEnd(void*) {
  audio::stop();
  const Event e = Event::Ended;
  xQueueSend(g_events, &e, portMAX_DELAY);
}

// --- Touch ---------------------------------------------------------------

void touchTask(void*) {
  // The touch chip answers only a moment after its reset: its first reads
  // fail for ~0.1 s, so keep those errors out of the log.
  esp_log_level_set("SPD2010", ESP_LOG_NONE);
  esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
  for (int i = 0; i < 50; ++i) {
    board::touched();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  esp_log_level_set("SPD2010", ESP_LOG_ERROR);
  esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_ERROR);

  while (true) {
    int x = 0;
    int y = 0;
    const bool down = board::touched(&x, &y);
    if (ui::onTouch(down, x, y) == ui::Action::Next) {
      const Event e = Event::Next;
      xQueueSend(g_events, &e, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// --- Power button ---------------------------------------------------------

// Holding PWR switches the board off. A hold that was already going on at
// start-up (the press that switched it on) does not count.
void powerTask(void*) {
  constexpr TickType_t kHold = pdMS_TO_TICKS(CONFIG_PLAYER_POWER_HOLD_MS);
  bool armed = false;  // seen released since start-up
  TickType_t pressed_at = 0;
  bool was_down = false;
  while (true) {
    const bool down = board::powerButtonDown();
    const TickType_t now = xTaskGetTickCount();
    if (!down) {
      armed = true;
    } else if (!was_down) {
      pressed_at = now;
    } else if (armed && now - pressed_at >= kHold) {
      // Silence and stop the video, let the last frame reach the screen,
      // then switch off.
      g_off = true;
      ui::setMutedNow();
      avi_player_play_stop(g_player);
      vTaskDelay(pdMS_TO_TICKS(200));
      audio::stop();
      board::powerOff();
    }
    was_down = down;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// --- Playback control ------------------------------------------------------

// Starts `path`; false if it cannot even be opened.
bool start(const std::string& path) {
  video::reset();
  ESP_LOGI(kTag, "playing %s", path.c_str() + strlen(kMount) + 1);
  return avi_player_play_from_file(g_player, path.c_str()) == ESP_OK;
}

// Asks the player to stop; its end callback then posts Ended. The player
// only accepts a stop once it has started parsing, so retry briefly.
void requestStop() {
  for (int i = 0; i < 25; ++i) {
    if (avi_player_play_stop(g_player) == ESP_OK) {
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Waits for a card with videos ("Video not found" on screen meanwhile);
// returns the list.
std::vector<std::string> waitForVideos() {
  bool shown = false;
  while (true) {
    if (mountCard(shown)) {
      std::vector<std::string> files = findVideos();
      if (!files.empty()) {
        return files;
      }
      unmountCard();  // remount on the next try: the card may be swapped
    }
    if (!shown) {
      ui::showNotFound();
      ESP_LOGW(kTag, "video not found: waiting for a card with .avi files");
      shown = true;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

}  // namespace

extern "C" void app_main(void) {
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs);
  ESP_ERROR_CHECK(ui::init());
  ESP_ERROR_CHECK(board::init());
  ESP_ERROR_CHECK(video::init());
  ESP_ERROR_CHECK(audio::init());

  g_events = xQueueCreate(8, sizeof(Event));
  avi_player_config_t cfg = {};
  cfg.buffer_size = kChunkBytes;  // malloc'd: lands in PSRAM
  cfg.video_cb = &onVideo;
  cfg.audio_cb = &onAudio;
  cfg.audio_set_clock_cb = &onAudioFormat;
  cfg.avi_play_end_cb = &onEnd;
  cfg.priority = 5;
  cfg.coreID = 1;  // decoding runs here while the display task on core 0 sends the previous frame
  cfg.stack_size = 12 * 1024;
  cfg.stack_in_psram = false;  // the SD card is read from this task
  ESP_ERROR_CHECK(avi_player_init(cfg, &g_player));
  xTaskCreatePinnedToCore(&touchTask, "touch", 4096, nullptr, 4, nullptr, 0);
  xTaskCreatePinnedToCore(&powerTask, "power", 3072, nullptr, 7, nullptr, 0);

  std::vector<std::string> files = waitForVideos();
  size_t index = 0;
  size_t failures = 0;  // files in a row that showed no frame
  bool skip = false;    // a touch asked for the next file
  bool playing = start(files[index]);
  if (!playing) {
    const Event e = Event::Ended;
    xQueueSend(g_events, &e, 0);
  }

  while (true) {
    Event e;
    xQueueReceive(g_events, &e, portMAX_DELAY);
    if (e == Event::Next) {
      if (playing && !skip) {
        ESP_LOGI(kTag, "next video");
        skip = true;
        requestStop();
      }
      continue;
    }

    // Ended: loop the same file, or move on (menu "next", play-all mode, or
    // a file that would not play).
    playing = false;
    if (g_off) {
      continue;  // switching off
    }
    const bool failed = video::framesShown() == 0 && !skip;
    failures = failed ? failures + 1 : 0;
    if (failed) {
      ESP_LOGW(kTag, "nothing shown from %s: skipped", files[index].c_str() + strlen(kMount) + 1);
    }
    if (failures >= files.size()) {
      // Nothing on the card plays (or it was pulled out): look again.
      ui::showNotFound();
      vTaskDelay(pdMS_TO_TICKS(3000));
      unmountCard();
      files = waitForVideos();
      index = 0;
      failures = 0;
    } else if (skip || failed || ui::settings().mode == ui::Mode::PlayAll) {
      index = (index + 1) % files.size();
    }
    skip = false;
    playing = start(files[index]);
    if (!playing) {
      const Event again = Event::Ended;
      xQueueSend(g_events, &again, 0);
    }
  }
}
