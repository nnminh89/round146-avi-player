#!/usr/bin/env bash
# Converts videos to the AVI format the round player plays:
#   412x412 (cropped to fill the round screen), MJPEG baseline, 20 fps,
#   16-bit PCM mono 22050 Hz sound.
# Usage: tools/convert.sh in1.mp4 [in2.mov ...]   -> in1.avi next to each input
# Options (environment): FPS=15 QUALITY=8 (2 = best/biggest .. 31 = worst) MUTE=1
set -euo pipefail
FPS="${FPS:-20}"
QUALITY="${QUALITY:-6}"
for in in "$@"; do
  out="${in%.*}.avi"
  audio=(-c:a pcm_s16le -ar 22050 -ac 1)
  [[ "${MUTE:-0}" == 1 ]] && audio=(-an)
  ffmpeg -hide_banner -loglevel error -y -i "$in" \
    -vf "fps=${FPS},scale=412:412:force_original_aspect_ratio=increase,crop=412:412" \
    -c:v mjpeg -q:v "$QUALITY" -pix_fmt yuvj420p "${audio[@]}" "$out"
  echo "$out"
done
