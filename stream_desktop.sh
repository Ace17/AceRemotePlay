#!/usr/bin/env bash
set -euo pipefail
ffmpeg -loglevel 30 \
  -f x11grab -s 320x240 -framerate 30 -i :0.0+0,0 \
  -thread_queue_size 128 \
  -f lavfi -i "sine=frequency=440" \
  -f lavfi -i "sine=frequency=660" \
  -filter_complex amerge \
  -acodec aac \
  -vcodec libx264 -x264-params keyint=30:scenecut=0 -vb 500k \
  -tune zerolatency \
  -f mpegts \
  udp://10.1.1.6:1234 </dev/null
