#!/usr/bin/env bash
export BIN=bin
set -euo pipefail
make -j`nproc`
readonly HOOKPATH=$(realpath $BIN)/acehook.so
#(
#cd deeep
#LD_PRELOAD=$HOOKPATH ./deeep.x86_64 0
#)
SCALE=1
LD_PRELOAD=$HOOKPATH mednafen -snes.xscale $SCALE -snes.yscale $SCALE -video.fs 0 "/home/ace/Roms/snes/Super Bomberman 2 (USA).zip"
# ~/Roms/snes/Super\ Mario\ All-Stars\ \(U\)\ \[\!\].zip
