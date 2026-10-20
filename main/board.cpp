#include "board.h"

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_io_expander_tca9554.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_spd2010.h"
#include "esp_lcd_touch_spd2010.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace board {
namespace {

constexpr const char* kTag = "board";

// Pins (Waveshare wiki + their xiaozhi board file).
constexpr gpio_num_t kPowerHold = GPIO_NUM_7;    // keeps the battery switch on
constexpr gpio_num_t kPowerButton = GPIO_NUM_6;  // PWR, low while pressed
constexpr gpio_num_t kI2cScl = GPIO_NUM_10;
constexpr gpio_num_t kI2cSda = GPIO_NUM_11;
constexpr gpio_num_t kTouchInt = GPIO_NUM_4;
constexpr gpio_num_t kLcdClk = GPIO_NUM_40;
constexpr gpio_num_t kLcdCs = GPIO_NUM_21;
constexpr gpio_num_t kLcdD0 = GPIO_NUM_46;
constexpr gpio_num_t kLcdD1 = GPIO_NUM_45;
constexpr gpio_num_t kLcdD2 = GPIO_NUM_42;
constexpr gpio_num_t kLcdD3 = GPIO_NUM_41;
constexpr gpio_num_t kBacklight = GPIO_NUM_5;
constexpr spi_host_device_t kLcdHost = SPI2_HOST;
constexpr int kLcdClockHz = 40 * 1000 * 1000;

// TCA9554 outputs. Board revisions differ on which of EXIO0..2 reset the
// panel and the touch chip, so all three are pulsed; EXIO3 is the SD card's
// D3 and must be high for 1-bit SD mode.
constexpr uint32_t kResetPins = IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2;
constexpr uint32_t kSdD3Pin = IO_EXPANDER_PIN_NUM_3;

// Rows per DMA band: 412 x 24 x 2 = ~20 KB, two of them in internal RAM.
constexpr int kBandRows = 24;
constexpr size_t kBandBytes = kWidth * kBandRows * 2;

i2c_master_bus_handle_t g_i2c = nullptr;
esp_lcd_panel_handle_t g_panel = nullptr;
esp_lcd_touch_handle_t g_touch = nullptr;
uint8_t* g_band[2] = {};
int g_next_band = 0;
// Counts free band buffers; the "colour sent" callback gives one back.
SemaphoreHandle_t g_band_free = nullptr;

bool IRAM_ATTR onColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(g_band_free, &woken);
  return woken == pdTRUE;
}

esp_err_t initExpander() {
  esp_io_expander_handle_t io = nullptr;
  ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_tca9554(g_i2c, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io), kTag,
                      "TCA9554");
  ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io, kResetPins | kSdD3Pin, IO_EXPANDER_OUTPUT), kTag, "EXIO dir");
  ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io, kSdD3Pin, 1), kTag, "SD D3");
  esp_io_expander_set_level(io, kResetPins, 1);
  vTaskDelay(pdMS_TO_TICKS(20));
  esp_io_expander_set_level(io, kResetPins, 0);
  vTaskDelay(pdMS_TO_TICKS(50));
  esp_io_expander_set_level(io, kResetPins, 1);
  vTaskDelay(pdMS_TO_TICKS(200));
  return ESP_OK;
}

esp_err_t initDisplay() {
  // Same as SPD2010_PANEL_BUS_QSPI_CONFIG / _IO_QSPI_CONFIG (C-only macros).
  spi_bus_config_t bus = {};
  bus.sclk_io_num = kLcdClk;
  bus.data0_io_num = kLcdD0;
  bus.data1_io_num = kLcdD1;
  bus.data2_io_num = kLcdD2;
  bus.data3_io_num = kLcdD3;
  bus.max_transfer_sz = kBandBytes;
  ESP_RETURN_ON_ERROR(spi_bus_initialize(kLcdHost, &bus, SPI_DMA_CH_AUTO), kTag, "QSPI bus");

  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.cs_gpio_num = kLcdCs;
  io_cfg.dc_gpio_num = GPIO_NUM_NC;
  io_cfg.spi_mode = 3;
  io_cfg.pclk_hz = kLcdClockHz;
  io_cfg.trans_queue_depth = 10;
  io_cfg.on_color_trans_done = onColorDone;
  io_cfg.lcd_cmd_bits = 32;
  io_cfg.lcd_param_bits = 8;
  io_cfg.flags.quad_mode = true;
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kLcdHost), &io_cfg, &io), kTag,
                      "panel IO");

  spd2010_vendor_config_t vendor = {};
  vendor.flags.use_qspi_interface = 1;
  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = GPIO_NUM_NC;  // reset through the expander
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_cfg.bits_per_pixel = 16;
  panel_cfg.vendor_config = &vendor;
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_spd2010(io, &panel_cfg, &g_panel), kTag, "SPD2010");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(g_panel), kTag, "panel reset");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(g_panel), kTag, "panel init");

  g_band_free = xSemaphoreCreateCounting(2, 2);
  ESP_RETURN_ON_FALSE(g_band_free, ESP_ERR_NO_MEM, kTag, "semaphore");
  for (auto& band : g_band) {
    band = static_cast<uint8_t*>(heap_caps_malloc(kBandBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    ESP_RETURN_ON_FALSE(band, ESP_ERR_NO_MEM, kTag, "band buffer");
  }
  fill(0x0000);
  return esp_lcd_panel_disp_on_off(g_panel, true);
}

esp_err_t initBacklight() {
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_10_BIT;
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = 20'000;
  timer.clk_cfg = LEDC_AUTO_CLK;
  ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), kTag, "backlight timer");
  ledc_channel_config_t ch = {};
  ch.gpio_num = kBacklight;
  ch.speed_mode = LEDC_LOW_SPEED_MODE;
  ch.channel = LEDC_CHANNEL_0;
  ch.timer_sel = LEDC_TIMER_0;
  ch.duty = 0;
  return ledc_channel_config(&ch);
}

esp_err_t initTouch() {
  esp_lcd_panel_io_handle_t io = nullptr;
  // Same as ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG (C-only macro).
  esp_lcd_panel_io_i2c_config_t io_cfg = {};
  io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_SPD2010_ADDRESS;
  io_cfg.scl_speed_hz = 100000;
  io_cfg.control_phase_bytes = 1;
  io_cfg.flags.disable_control_phase = 1;
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(g_i2c, &io_cfg, &io), kTag, "touch IO");
  esp_lcd_touch_config_t cfg = {};
  cfg.x_max = kWidth;
  cfg.y_max = kHeight;
  cfg.rst_gpio_num = GPIO_NUM_NC;  // reset through the expander
  cfg.int_gpio_num = kTouchInt;
  return esp_lcd_touch_new_i2c_spd2010(io, &cfg, &g_touch);
}

}  // namespace

esp_err_t init() {
  // Hold the power switch on first: on battery the board turns off otherwise.
  // An earlier firmware froze every pad before its "off" sleep, and that
  // freeze outlives resets (it kept the backlight dark): undo it.
  gpio_deep_sleep_hold_dis();
  for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
    if (GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
      gpio_hold_dis(static_cast<gpio_num_t>(pin));
    }
  }
  gpio_reset_pin(kPowerHold);
  gpio_set_direction(kPowerHold, GPIO_MODE_OUTPUT);
  gpio_set_level(kPowerHold, 1);
  gpio_config_t button = {};
  button.pin_bit_mask = 1ULL << kPowerButton;
  button.mode = GPIO_MODE_INPUT;
  button.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&button);

  i2c_master_bus_config_t i2c = {};
  i2c.i2c_port = I2C_NUM_0;
  i2c.sda_io_num = kI2cSda;
  i2c.scl_io_num = kI2cScl;
  i2c.clk_source = I2C_CLK_SRC_DEFAULT;
  i2c.glitch_ignore_cnt = 7;
  i2c.flags.enable_internal_pullup = true;
  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c, &g_i2c), kTag, "I2C");

  ESP_RETURN_ON_ERROR(initExpander(), kTag, "expander");
  ESP_RETURN_ON_ERROR(initBacklight(), kTag, "backlight");
  ESP_RETURN_ON_ERROR(initDisplay(), kTag, "display");
  setBacklight(CONFIG_PLAYER_BACKLIGHT);
  // The video still plays without touch; it just cannot be skipped.
  const esp_err_t touch = initTouch();
  if (touch != ESP_OK) {
    ESP_LOGE(kTag, "touch not available: %s", esp_err_to_name(touch));
  }
  return ESP_OK;
}

void draw(int x, int y, int w, int h, const uint8_t* src, int stride, Overlay overlay) {
  // Align to the panel's 4-column groups and clip to the screen.
  const int x0 = std::max(0, x) & ~3;
  const int skip = x0 - x;  // columns of src left of the screen (x < 0)
  w = std::min(w - std::max(0, skip), kWidth - x0) & ~3;
  if (w <= 0 || h <= 0) {
    return;
  }
  const size_t row_bytes = static_cast<size_t>(w) * 2;
  const int rows_per_band = std::min<int>(kBandRows, static_cast<int>(kBandBytes / row_bytes));
  for (int row = 0; row < h; row += rows_per_band) {
    const int yy = y + row;
    const int n = std::min(rows_per_band, h - row);
    if (yy + n <= 0 || yy >= kHeight) {
      continue;
    }
    xSemaphoreTake(g_band_free, portMAX_DELAY);
    uint8_t* band = g_band[g_next_band];
    g_next_band ^= 1;
    for (int r = 0; r < n; ++r) {
      memcpy(band + r * row_bytes, src + (static_cast<size_t>(row + r) * stride + std::max(0, skip)) * 2, row_bytes);
    }
    if (overlay) {
      overlay(band, x0, yy, w, n);
    }
    if (esp_lcd_panel_draw_bitmap(g_panel, x0, yy, x0 + w, yy + n, band) != ESP_OK) {
      xSemaphoreGive(g_band_free);  // nothing was queued
    }
  }
}

void fill(uint16_t rgb565) {
  const uint8_t hi = rgb565 >> 8;
  const uint8_t lo = rgb565 & 0xFF;
  for (int y = 0; y < kHeight; y += kBandRows) {
    const int n = std::min(kBandRows, kHeight - y);
    xSemaphoreTake(g_band_free, portMAX_DELAY);
    uint8_t* band = g_band[g_next_band];
    g_next_band ^= 1;
    for (size_t i = 0; i < kBandBytes; i += 2) {
      band[i] = hi;  // the panel takes big-endian RGB565
      band[i + 1] = lo;
    }
    if (esp_lcd_panel_draw_bitmap(g_panel, 0, y, kWidth, y + n, band) != ESP_OK) {
      xSemaphoreGive(g_band_free);
    }
  }
}

bool powerButtonDown() {
  return gpio_get_level(kPowerButton) == 0;
}

void powerOff() {
  ESP_LOGI(kTag, "power off");
  setBacklight(0);
  fill(0x0000);
  esp_lcd_panel_disp_on_off(g_panel, false);
  // Wait for the button to be let go, so it does not wake us right away.
  while (powerButtonDown()) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  vTaskDelay(pdMS_TO_TICKS(100));
  gpio_set_level(kPowerHold, 0);  // on battery the power goes here
  vTaskDelay(pdMS_TO_TICKS(500));

  // Still running: USB keeps the board powered, so it cannot switch off.
  // Stay dark and silent (the caller stopped the sound) until PWR is
  // pressed, then start over as if switched on.
  ESP_LOGI(kTag, "on USB power: screen off until PWR is pressed");
  while (!powerButtonDown()) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  esp_restart();
}

void setBacklight(int percent) {
  percent = std::clamp(percent, 0, 100);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, static_cast<uint32_t>(percent * 1023 / 100));
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

bool touched(int* x, int* y) {
  if (!g_touch || esp_lcd_touch_read_data(g_touch) != ESP_OK) {
    return false;
  }
  esp_lcd_touch_point_data_t points[1];
  uint8_t count = 0;
  if (esp_lcd_touch_get_data(g_touch, points, &count, 1) != ESP_OK) {
    return false;
  }
  if (count > 0) {
    if (x) *x = points[0].x;
    if (y) *y = points[0].y;
  }
  return count > 0;
}

}  // namespace board
