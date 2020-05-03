#!/usr/bin/env bash
export BIN=bin
set -euo pipefail
make -j`nproc`
readonly HOOKPATH=$(realpath $BIN)/acehook.so
SCALE=1
ROM='/home/ace/Roms/snes/Super Bomberman 2 (USA).zip'
ROM='/home/ace/Roms/snes/Super Mario All-Stars (U) [!].zip'
ROM='/home/ace/Roms/snes/Super Mario Kart (Europe).zip'
LD_PRELOAD=$HOOKPATH mednafen -snes.xscale $SCALE -snes.yscale $SCALE -video.fs 0 "$ROM"
