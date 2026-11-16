#!/bin/sh
# Cross-compile from Linux with MinGW-w64:  sudo apt install g++-mingw-w64-x86-64-posix
set -e
cd "$(dirname "$0")"
mkdir -p build
CXX=${CXX:-x86_64-w64-mingw32-g++-posix}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
$WINDRES -I res res/app.rc -O coff -o build/res.o
$CXX -std=c++17 -Os -s -municode -mwindows -static -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables \
  -ffunction-sections -fdata-sections -Wl,--gc-sections -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers -Wno-misleading-indentation \
  src/app.cpp src/arrange.cpp src/settings.cpp src/widgets.cpp src/media.cpp src/weather.cpp src/events.cpp src/daypop.cpp src/net.cpp src/glass.cpp src/util.cpp src/json.cpp src/pet/pet_rig.cpp src/pet/pet_draw.cpp src/pet/pet_brain.cpp src/pet/pet_input.cpp build/res.o \
  -o build/PrismWidgets.exe \
  -ld2d1 -ldwrite -lwindowscodecs -lole32 -luuid -lshell32 -lshlwapi -lshcore -ldwmapi -lwtsapi32 -lpsapi -lruntimeobject -lwinhttp -ladvapi32 -lgdi32 -luser32
ls -la build/PrismWidgets.exe
