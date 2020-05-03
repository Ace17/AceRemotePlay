#!/usr/bin/env bash
set -euo pipefail
ffmpeg -loglevel 40 \
  -f x11grab -s 320x240 -framerate 25 -i :0.0+0,0 \
  -thread_queue_size 128 \
  -f alsa -ac 2 -ar 48000 -i hw:0,2 \
  -acodec aac \
  -vcodec h264 -vb 500k \
  -tune zerolatency \
  -f mpegts \
  udp://10.1.1.1:1234 </dev/null
