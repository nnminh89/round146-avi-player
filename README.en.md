# Round AVI Player — Waveshare ESP32-S3 1.46" Round Display

[Tiếng Việt](README.md) | **English**

Firmware that plays **AVI videos from a microSD card** on the 412×412 round screen of the
**Waveshare ESP32-S3-Touch-LCD-1.46 / 1.46B**. Insert a card with videos and it starts playing; tap the screen for an
icon menu (sound on/off, next video, loop one video / play all, volume).

![Menu (normal / muted) and the "Video not found" screen](docs/screens.png)

## Features

- Finds every `.avi` file on the card (subfolders too, 4 levels deep), sorted by name, skipping hidden files — including
  the `._name.avi` junk files macOS writes when copying to a FAT card.
- Plays 412×412 MJPEG video at up to ~30 frames per second, with PCM sound on the onboard speaker.
- **Tap the screen** → a menu appears over the video (which keeps playing). Icons only, no text:

  | Button | Action |
  |---|---|
  | 🔊 / 🔇 (left) | Sound off / on — the button turns **red** while muted |
  | ⏭ (middle) | Next video; after the last one, back to the first |
  | 🔂 / 🔁 (right) | 🔂 **loop one video** / 🔁 **play all videos** in turn, then start over |
  | Slider (bottom) | Volume — drag sideways |

  Tap outside the buttons, or wait 4 seconds, and the menu hides. Mute, mode and volume are saved in flash (kept across
  power cycles).
- **PWR button**: on / off only (no sleep mode). On battery: press to switch on, **hold for 2 seconds to switch off**.
- No card / no video → black screen with white **"Video not found"**; the card is checked again every 3 seconds, so it
  starts playing as soon as a card with videos goes in.

## Specifications

| Item | Value |
|---|---|
| Board | Waveshare ESP32-S3-Touch-LCD-1.46 / 1.46B |
| Chip | ESP32-S3R8 — dual core 240 MHz, 8 MB octal PSRAM, 16 MB flash |
| Display | 1.46" round IPS 412×412, SPD2010 driver, QSPI at 40 MHz |
| Touch | SPD2010 (I2C 0x53) |
| Audio | PCM5101 (I2S) → onboard speaker |
| Card | microSD, SDMMC 1-bit at 40 MHz, FAT32 / exFAT |
| Video | AVI, **baseline MJPEG** pictures, **whole-number** fps (15/20/24/30), best at **412×412** |
| Soundtrack | **16-bit PCM**, mono/stereo, any sample rate (MP3/AAC: picture only, no sound) |
| Measured on the board | 412×412 decode ≈ 27 ms/frame (enough for ~30 fps); a 24 fps video with sound runs at ~23 fps |
| Firmware | ESP-IDF **6.0.2**, C++17, ~450 KB |

## 1. Prepare the videos

The board cannot decode MP4/H.264/HEVC → convert to **AVI with MJPEG + PCM**. You need `ffmpeg`:

| OS | Install ffmpeg |
|---|---|
| macOS | `brew install ffmpeg` |
| Ubuntu / Debian | `sudo apt install ffmpeg` |
| Windows | `winget install ffmpeg` (or download from ffmpeg.org) |

### macOS / Linux — with the included script

```bash
tools/convert.sh video1.mp4 video2.mov        # writes video1.avi, video2.avi next to the originals
FPS=24 tools/convert.sh clip.mp4              # smoother (default is 20 fps)
FPS=15 QUALITY=8 tools/convert.sh clip.mp4    # lighter, if playback stutters
MUTE=1 tools/convert.sh clip.mp4              # drop the sound
```

| Variable | Default | Meaning |
|---|---|---|
| `FPS` | 20 | Frames per second (whole number). 24 runs well at 412×412; 30 is close to the limit |
| `QUALITY` | 6 | JPEG quality: 2 = best / biggest file … 31 = worst / smallest |
| `MUTE` | 0 | `1` = no sound |

The script crops the video to **412×412, filling the screen** (non-square videos lose their sides and keep the middle),
MJPEG `yuvj420p` pictures, 16-bit mono PCM sound at 22050 Hz.

### Windows (or by hand) — the equivalent ffmpeg command

```bash
ffmpeg -i input.mp4 -vf "fps=24,scale=412:412:force_original_aspect_ratio=increase,crop=412:412" -c:v mjpeg -q:v 6 -pix_fmt yuvj420p -c:a pcm_s16le -ar 22050 -ac 1 output.avi
```

Without sound: replace `-c:a pcm_s16le -ar 22050 -ac 1` with `-an`.

### Tips

- Round screen: **the 4 corners of the square frame are hidden** → keep the important part in the middle.
- Videos smaller than 412×412 are centred (black border); larger ones are scaled down by the firmware — slower, so
  convert to 412×412 beforehand.
- A 30-second 412×412 24 fps video with sound is about 15 MB.
- If the picture runs slower than real time or the sound crackles: lower `FPS` or raise `QUALITY`.

## 2. Copy the videos to the microSD card

1. Format the card as **FAT32** (cards over 32 GB: **exFAT**).
2. Copy the `.avi` files to the card — the root or any subfolder. Non-ASCII file names are fine.
3. **Play order = name order** (case-insensitive). To choose the order, name the files `01_...avi`, `02_...avi`, …
4. macOS: copying with Finder is fine (the `._...` files are skipped); **Eject** the card before pulling it out.
5. Insert the card into the board (also while it is running — playback starts within 3 seconds).

## 3. Build & flash the firmware

### Install ESP-IDF 6.0.2 (once)

macOS / Linux:

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v6.0.2 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6.0.2
cd esp-idf-v6.0.2 && ./install.sh esp32s3
```

Windows: use the **ESP-IDF Installer** from Espressif (pick v6.0.2), then open "ESP-IDF 6.0 CMD".

### Build

```bash
. ~/esp/esp-idf-v6.0.2/export.sh          # in every new terminal
git clone https://github.com/nnminh89/round146-avi-player.git
cd round146-avi-player
idf.py set-target esp32s3                 # first time only
idf.py build
```

The first build downloads Espressif's libraries (versions locked in `dependencies.lock`):
`esp_lcd_spd2010`, `esp_lcd_touch_spd2010`, `esp_io_expander_tca9554`, `avi_player`, `esp_new_jpeg`.

### Flash

Connect the board through its USB-C port, then:

```bash
idf.py -p /dev/cu.usbmodem* flash monitor     # macOS
idf.py -p /dev/ttyACM0 flash monitor          # Linux
idf.py -p COM5 flash monitor                  # Windows (find the COM number in Device Manager)
```

- Leave `monitor` with `Ctrl+]`.
- Port not found / flashing fails: hold **BOOT**, press **RESET** (or plug USB in while holding BOOT), release BOOT and
  flash again. Press RESET afterwards to run.

### Settings (`idf.py menuconfig` → **Round AVI player**)

| Option | Default |
|---|---|
| Backlight (%) | 80 |
| Speaker volume on first boot (%) — afterwards the menu slider sets it | 70 |
| Hold PWR this long to switch off (ms) | 2000 |

After editing `sdkconfig.defaults`, delete `sdkconfig` and build again to apply it.

## 4. Usage

| Action | Result |
|---|---|
| Power on (battery: press PWR; or plug in USB) | Scans the card, plays the first video |
| Tap the screen | Opens the menu |
| Left / middle / right menu button | Sound on-off / next video / loop one – play all |
| Drag the bottom slider | Volume (squared scale — sounds even to the ear) |
| Tap outside the buttons / wait 4 s | Hides the menu |
| Hold PWR for 2 s | Switches off. On USB power it cannot cut the power: the screen goes dark and the video stops; press PWR to start again |

## 5. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| "Video not found" | No card, card not FAT32/exFAT, no `.avi` file, or files are not MJPEG → convert again with `tools/convert.sh` |
| Picture but no sound | The soundtrack is MP3/AAC → convert again with the script (PCM); or muted / volume 0 in the menu |
| Video runs slow, sound crackles | Lower `FPS` (20 or 15), raise `QUALITY`, or make sure the video is exactly 412×412 |
| Dark screen although the log says it is playing | Backlight pin stuck — flash the latest firmware (it releases the pins at start-up) |
| The left button does what the right one should | Touch axis mirrored on another board revision — report it so an axis flip option can be added |
| Cannot flash | Enter download mode with BOOT + RESET (see above) |

Details (video list, real frame rate, decode time, card errors) are in the `idf.py monitor` log:

```
I (933) player: 3 video(s) on the card
I (933) player: playing 01_intro.avi
I (1080) audio: sound 22050 Hz, mono
I (5907) video: 22.9 fps shown, 412x412, decode 27.7 ms/frame
```

## Hardware — pins used

From the Waveshare wiki and the xiaozhi firmware for this board.

| Function | Pins |
|---|---|
| SPD2010 display, QSPI | CLK 40, CS 21, D0–D3 46/45/42/41, backlight 5 (PWM) |
| I2C (SPD2010 touch, TCA9554) | SDA 11, SCL 10; touch interrupt 4 |
| TCA9554 (I2C) | EXIO0–2 display + touch reset; EXIO3 = SD card D3 (held high) |
| microSD (SDMMC 1-bit) | CLK 14, CMD 17, D0 16 |
| Speaker (PCM5101, I2S) | BCK 48, LRCK 38, DIN 47 |
| PWR button | GPIO6 (low while pressed) |
| Battery power latch | GPIO7 = 1 while running, back to 0 to switch off |

## Code layout

```
main/
  main.cpp        card mounting, .avi scan, play / loop / next control, PWR button
  board.*         power, I2C, TCA9554, SPD2010 display (drawn through 2 internal-RAM DMA buffers + an overlay
                  hook), backlight, touch, power-off
  video.*         MJPEG → RGB565 into 2 PSRAM frames; a display task sends one while the next is decoded
  audio.*         PCM → I2S → PCM5101, volume / mute
  ui.*            touch menu: 3 icon buttons + volume slider, anti-aliased drawing blended over the video,
                  saved in NVS; the "Video not found" screen
  icons.h         icons + text (generated by tools/gen_icons.py — do not edit by hand)
tools/
  convert.sh      converts videos to playable AVI
  gen_icons.py    draws the icons / text → main/icons.h (preview: tools/icons_preview.png)
  fonts/          Be Vietnam Pro Bold (SIL OFL)
docs/screens.png  pictures of the menu and the error screen (drawn by ui.cpp itself)
```

### Technical notes

- **The SPD2010 only takes columns in groups of 4**: every drawn area has x and width as multiples of 4.
- Frames are decoded into PSRAM, then copied in 24-row bands to internal RAM for the QSPI DMA (no DMA reads from PSRAM).
- `avi_player` paces playback by the fps in the header (whole part only) → 23.976 fps becomes 23.
- The `esp_new_jpeg` decoder only scales in steps of 8 (412 is not one) → large videos are scaled to the closest fit,
  e.g. 408×408.
- The menu does not use LVGL: the menu image (RGB565 + alpha) is redrawn only when its state changes and is blended into
  each band on its way to the screen.
- No deep sleep and no GPIO hold when switching off: a held pin stays held even through a reset (it once kept the
  backlight dark).

## Third-party licenses

- Espressif libraries (`esp_lcd_spd2010`, `esp_lcd_touch_spd2010`, `esp_io_expander_tca9554`, `avi_player`,
  `esp_new_jpeg`): downloaded from the ESP Component Registry, under each package's license.
- **Be Vietnam Pro** font — SIL Open Font License 1.1 (`tools/fonts/BeVietnamPro-Bold.OFL.txt`).
