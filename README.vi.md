# Round AVI Player — Waveshare ESP32-S3 1.46" Round Display

[English](README.md) | **Tiếng Việt**

Firmware phát video **AVI từ thẻ microSD** lên màn hình tròn 412×412 của board
**Waveshare ESP32-S3-Touch-LCD-1.46 / 1.46B**. Cắm thẻ có video vào là tự phát; chạm màn hình mở menu điều khiển
bằng icon (tắt/bật tiếng, video kế tiếp, lặp 1 video / phát tất cả, âm lượng).

![Menu (bình thường / đã tắt tiếng) và màn "Video not found"](docs/screens.png)

## Tính năng

- Tự quét mọi file `.avi` trên thẻ (cả thư mục con, sâu 4 cấp), sắp theo tên, bỏ qua file ẩn — kể cả file rác `._ten.avi`
  mà macOS tạo khi chép vào thẻ FAT.
- Phát video MJPEG 412×412 tới ~30 khung/giây, kèm tiếng PCM ra loa trên mạch.
- **Chạm màn hình** → menu đè lên video (video vẫn chạy), chỉ có icon, không chữ:

  | Nút | Tác dụng |
  |---|---|
  | 🔊 / 🔇 (trái) | Tắt / bật tiếng — nút chuyển **đỏ** khi đang tắt |
  | ⏭ (giữa) | Sang video kế tiếp; hết danh sách thì quay lại video đầu |
  | 🔂 / 🔁 (phải) | 🔂 **lặp 1 video** / 🔁 **phát lần lượt tất cả** rồi quay lại đầu |
  | Thanh trượt (dưới) | Âm lượng — kéo ngang |

  Chạm ra ngoài các nút, hoặc 4 giây không chạm → menu tự ẩn. Tắt tiếng, chế độ, âm lượng được lưu vào flash (tắt nguồn
  vẫn nhớ).
- **Nút PWR**: chỉ có mở / tắt (không có chế độ ngủ). Chạy pin: bấm để mở, **giữ 2 giây để tắt** hẳn.
- Không có thẻ / không có video → màn đen, chữ trắng **"Video not found"**; tự kiểm tra lại mỗi 3 giây, cắm thẻ vào là
  phát.

## Thông số

| Hạng mục | Giá trị |
|---|---|
| Board | Waveshare ESP32-S3-Touch-LCD-1.46 / 1.46B |
| Chip | ESP32-S3R8 — 2 nhân 240 MHz, 8 MB PSRAM octal, 16 MB flash |
| Màn hình | 1.46" tròn IPS 412×412, driver SPD2010, QSPI 40 MHz |
| Cảm ứng | SPD2010 (I2C 0x53) |
| Âm thanh | PCM5101 (I2S) → loa trên mạch |
| Thẻ nhớ | microSD SDMMC 1-bit 40 MHz, FAT32 / exFAT |
| Video | AVI, hình **MJPEG baseline**, fps **nguyên** (15/20/24/30), tốt nhất **412×412** |
| Âm thanh trong video | **PCM 16-bit** mono/stereo, tần số bất kỳ (MP3/AAC: chỉ có hình, không tiếng) |
| Hiệu năng đo trên board | Giải mã 412×412 ≈ 27 ms/khung (đủ ~30 fps); video 24 fps có tiếng chạy ~23 fps |
| Firmware | ESP-IDF **6.0.2**, C++17, ~450 KB |

## Video mẫu

Muốn thử ngay: tải 4 video đã chuyển sẵn (412×412, 24 fps, 30 giây, 3 file có tiếng) ở
[Releases → Sample videos](https://github.com/nnminh89/round146-avi-player/releases/tag/samples), chép vào thẻ rồi
cắm vào board. Các clip có nhân vật Dragon Ball (© Bird Studio / Toei Animation): chỉ để thử máy phát,
không thuộc giấy phép của repo này, không phát tán lại.

## 1. Tạo video

Board không giải mã được MP4/H.264/HEVC → phải chuyển sang **AVI MJPEG + PCM**. Cần `ffmpeg`:

| Hệ điều hành | Cài ffmpeg |
|---|---|
| macOS | `brew install ffmpeg` |
| Ubuntu / Debian | `sudo apt install ffmpeg` |
| Windows | `winget install ffmpeg` (hoặc tải ở ffmpeg.org) |

### macOS / Linux — dùng script kèm theo

```bash
tools/convert.sh video1.mp4 video2.mov        # ra video1.avi, video2.avi cạnh file gốc
FPS=24 tools/convert.sh clip.mp4              # mượt hơn (mặc định 20 fps)
FPS=15 QUALITY=8 tools/convert.sh clip.mp4    # nhẹ hơn nếu bị giật
MUTE=1 tools/convert.sh clip.mp4              # bỏ tiếng
```

| Biến | Mặc định | Ý nghĩa |
|---|---|---|
| `FPS` | 20 | Khung/giây (số nguyên). 24 chạy tốt ở 412×412; 30 sát giới hạn |
| `QUALITY` | 6 | Chất lượng JPEG: 2 = đẹp nhất / file to … 31 = xấu nhất / nhẹ nhất |
| `MUTE` | 0 | `1` = bỏ tiếng |

Script cắt video thành **412×412 vừa khít màn** (video không vuông thì cắt bớt 2 bên, giữ phần giữa), hình MJPEG
`yuvj420p`, tiếng PCM 16-bit mono 22050 Hz.

### Windows (hoặc chạy tay) — lệnh ffmpeg tương đương

```bash
ffmpeg -i input.mp4 -vf "fps=24,scale=412:412:force_original_aspect_ratio=increase,crop=412:412" -c:v mjpeg -q:v 6 -pix_fmt yuvj420p -c:a pcm_s16le -ar 22050 -ac 1 output.avi
```

Bỏ tiếng: thay `-c:a pcm_s16le -ar 22050 -ac 1` bằng `-an`.

### Mẹo

- Màn tròn: **4 góc của khung vuông bị khuất** → để nội dung chính ở giữa.
- Video nhỏ hơn 412×412 được đặt giữa màn (viền đen); lớn hơn thì firmware tự thu nhỏ — chạy chậm hơn, nên chuyển về
  412×412 trước.
- Video 30 giây 412×412 24 fps có tiếng ≈ 15 MB.
- Nếu hình chạy chậm hơn thực tế hoặc tiếng rè: giảm `FPS` hoặc tăng `QUALITY`.

## 2. Chép video vào thẻ microSD

1. Format thẻ **FAT32** (thẻ > 32 GB dùng **exFAT**).
2. Chép các file `.avi` vào thẻ — để ở thư mục gốc hoặc thư mục con đều được. Tên file tiếng Việt được.
3. **Thứ tự phát = thứ tự tên** (không phân biệt hoa thường). Muốn sắp xếp: đặt tên `01_...avi`, `02_...avi`…
4. macOS: chép bằng Finder vẫn được (file `._...` tự bị bỏ qua); rút thẻ bằng **Eject** trước khi lấy ra.
5. Cắm thẻ vào board (có thể cắm lúc board đang chạy — tối đa 3 giây sau là phát).

## 3. Build & nạp firmware

### Cài ESP-IDF 6.0.2 (một lần)

macOS / Linux:

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v6.0.2 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6.0.2
cd esp-idf-v6.0.2 && ./install.sh esp32s3
```

Windows: dùng **ESP-IDF Installer** (chọn bản v6.0.2) ở trang Espressif, rồi mở "ESP-IDF 6.0 CMD".

### Build

```bash
. ~/esp/esp-idf-v6.0.2/export.sh          # mỗi lần mở terminal mới
git clone https://github.com/nnminh89/round146-avi-player.git
cd round146-avi-player
idf.py set-target esp32s3                 # lần đầu
idf.py build
```

Lần build đầu `idf.py` tự tải các thư viện của Espressif (phiên bản khoá trong `dependencies.lock`):
`esp_lcd_spd2010`, `esp_lcd_touch_spd2010`, `esp_io_expander_tca9554`, `avi_player`, `esp_new_jpeg`.

### Nạp (flash)

Cắm board bằng cổng USB-C, rồi:

```bash
idf.py -p /dev/cu.usbmodem* flash monitor     # macOS
idf.py -p /dev/ttyACM0 flash monitor          # Linux
idf.py -p COM5 flash monitor                  # Windows (xem số COM trong Device Manager)
```

- Thoát `monitor`: `Ctrl+]`.
- Không nhận cổng / nạp lỗi: giữ **BOOT**, bấm **RESET** (hoặc cắm lại USB khi đang giữ BOOT), nhả BOOT rồi nạp lại.
  Sau khi nạp bấm RESET để chạy.

### Tuỳ chỉnh (`idf.py menuconfig` → **Round AVI player**)

| Mục | Mặc định |
|---|---|
| Backlight (%) | 80 |
| Speaker volume on first boot (%) — sau đó dùng thanh trượt trên menu | 70 |
| Hold PWR this long to switch off (ms) | 2000 |

Sửa `sdkconfig.defaults` thì xoá file `sdkconfig` rồi build lại để áp dụng.

## 4. Sử dụng

| Thao tác | Kết quả |
|---|---|
| Bật nguồn (pin: bấm PWR; hoặc cắm USB) | Quét thẻ, phát video đầu tiên |
| Chạm màn hình | Mở menu |
| Nút trái / giữa / phải trên menu | Tắt-bật tiếng / video kế / lặp 1 – phát tất cả |
| Kéo thanh dưới | Âm lượng (thang bình phương — nghe đều tai) |
| Chạm ngoài nút / chờ 4 giây | Ẩn menu |
| Giữ PWR 2 giây | Tắt. Đang cắm USB thì không cắt được điện: màn tối, dừng video, bấm PWR để mở lại |

## 5. Xử lý sự cố

| Hiện tượng | Nguyên nhân / cách xử lý |
|---|---|
| "Video not found" | Không có thẻ, thẻ không phải FAT32/exFAT, hoặc không có file `.avi`, hoặc file không phải MJPEG → chuyển lại bằng `tools/convert.sh` |
| Có hình không có tiếng | Tiếng trong file là MP3/AAC → chuyển lại bằng script (PCM); hoặc đang tắt tiếng / âm lượng 0 trên menu |
| Video chạy chậm, tiếng rè | Giảm `FPS` (20 hoặc 15), tăng `QUALITY`, hoặc đảm bảo video đúng 412×412 |
| Màn đen nhưng log báo đang phát | Đèn nền bị khoá — nạp lại firmware mới nhất (đã xử lý lúc khởi động) |
| Bấm nút trái ra tác dụng nút phải | Trục cảm ứng bị lật trên bản board khác — báo lại để thêm cấu hình lật trục |
| Không nạp được | Vào chế độ nạp bằng BOOT + RESET (xem trên) |

Log chi tiết (danh sách video, fps thực tế, thời gian giải mã, lỗi thẻ) xem bằng `idf.py monitor`:

```
I (933) player: 3 video(s) on the card
I (933) player: playing 01_intro.avi
I (1080) audio: sound 22050 Hz, mono
I (5907) video: 22.9 fps shown, 412x412, decode 27.7 ms/frame
```

## Phần cứng — chân đã dùng

Theo wiki Waveshare và firmware xiaozhi cho board này.

| Chức năng | Chân |
|---|---|
| Màn SPD2010 QSPI | CLK 40, CS 21, D0–D3 46/45/42/41, đèn nền 5 (PWM) |
| I2C (cảm ứng SPD2010, TCA9554) | SDA 11, SCL 10; ngắt cảm ứng 4 |
| TCA9554 (I2C) | EXIO0–2 reset màn + cảm ứng; EXIO3 = D3 thẻ SD (giữ mức cao) |
| microSD (SDMMC 1-bit) | CLK 14, CMD 17, D0 16 |
| Loa (PCM5101, I2S) | BCK 48, LRCK 38, DIN 47 |
| Nút PWR | GPIO6 (mức thấp khi bấm) |
| Giữ nguồn pin | GPIO7 = 1 khi chạy, thả về 0 để tắt |

## Cấu trúc mã

```
main/
  main.cpp        gắn thẻ, quét .avi, điều khiển phát / lặp / chuyển video, nút PWR
  board.*         nguồn, I2C, TCA9554, màn SPD2010 (vẽ qua 2 buffer DMA trong RAM trong + hook trộn lớp phủ),
                  đèn nền, cảm ứng, tắt nguồn
  video.*         MJPEG → RGB565 vào 2 khung PSRAM; task hiển thị gửi khung này trong lúc giải mã khung sau
  audio.*         PCM → I2S → PCM5101, âm lượng / tắt tiếng
  ui.*            menu chạm: 3 nút icon + thanh âm lượng, vẽ khử răng cưa, trộn lên video, lưu NVS;
                  màn "Video not found"
  icons.h         icon + chữ (sinh bằng tools/gen_icons.py — đừng sửa tay)
tools/
  convert.sh      chuyển video sang AVI phát được
  gen_icons.py    vẽ icon / chữ → main/icons.h (xem trước: tools/icons_preview.png)
  fonts/          Be Vietnam Pro Bold (SIL OFL)
docs/screens.png  ảnh menu và màn báo lỗi (vẽ bằng chính code ui.cpp)
```

### Ghi chú kỹ thuật

- **SPD2010 chỉ nhận cột theo nhóm 4**: mọi vùng vẽ có x và chiều rộng là bội số của 4.
- Khung hình giải mã vào PSRAM, rồi chép từng dải 24 dòng sang RAM trong để DMA gửi QSPI (tránh DMA đọc PSRAM).
- `avi_player` tự canh nhịp theo fps trong header (lấy phần nguyên) → fps lẻ như 23.976 sẽ thành 23.
- Bộ giải mã `esp_new_jpeg` chỉ thu nhỏ theo bội số 8 (412 không phải) → video lớn được thu về khung vừa nhất,
  ví dụ 408×408.
- Menu không dùng LVGL: ảnh menu (RGB565 + alpha) chỉ vẽ lại khi trạng thái đổi, trộn vào từng dải lúc gửi màn.
- Không dùng deep sleep / giữ chân GPIO khi tắt: chân bị "hold" sống qua cả reset (từng làm kẹt đèn nền).

## Giấy phép thành phần bên thứ ba

- Thư viện Espressif (`esp_lcd_spd2010`, `esp_lcd_touch_spd2010`, `esp_io_expander_tca9554`, `avi_player`,
  `esp_new_jpeg`): tải qua ESP Component Registry, theo giấy phép của từng gói.
- Font **Be Vietnam Pro** — SIL Open Font License 1.1 (`tools/fonts/BeVietnamPro-Bold.OFL.txt`).
