#!/bin/bash
# build_mac_window_box64.sh — Same windowed macOS engine as build_mac_window.sh,
# but driven by the box64 translator backend (wg_blink_box64.c) instead of blink.
# The engine calls the same wg_blink_* API; wg_blink_box64.o provides those symbols
# against box64's x86-64 emu + ARM64 DynaRec. See memory/box64-pivot.md.
#
#   Tests/build_mac_window_box64.sh && \
#     WG_CPU=box64 /tmp/wineglass_box64/wineglass_window "<GameData>/…/Visage.exe"
#
# Requires Vendor/box64/build_mac/libbox64all.a (the ported box64 core) — build via:
#   cd Vendor/box64/build_mac && cmake .. -DM1=1 -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_C_FLAGS="-D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -Dsincos=__sincos" \
#     && cmake --build . -j4 -- -k   (then ar the core .o into libbox64all.a)
set -e
WG="$(cd "$(dirname "$0")/.." && pwd)"
BOX="$WG/Vendor/box64"
BUILD="${BUILD_DIR:-/tmp/wineglass_box64}"
mkdir -p "$BUILD"
SDK="$(xcrun --sdk macosx --show-sdk-path)"
CC="clang -arch arm64 -isysroot $SDK -O2 -g -Wno-everything"
ARCC="$CC -fobjc-arc"
LIBBOX="$BOX/build_mac/libbox64all.a"
if [ ! -f "$LIBBOX" ]; then echo "ERROR: $LIBBOX missing — build box64 core first (see header)"; exit 1; fi

# 1. box64 backend glue (compiled with box64 headers — they shadow system headers,
#    so this TU uses box64's flags only, like wg_blink_impl.o). Paths quoted for the
#    space in the repo dir name.
clang -arch arm64 -O2 -g -Wno-everything -DARM64 -DDYNAREC -DM1 \
  -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -Dsincos=__sincos \
  -I"$BOX/src/dynarec/arm64" -I"$BOX/src/include" -I"$BOX/src" -I"$BOX/src/emu" -I"$BOX/src/wrapped/generated" \
  -c "$WG/Sources/Core/wg_blink_box64.c" -o "$BUILD/wg_blink_box64.o"

# 2. zero-stubs for box64's unused Linux ELF/librarian layer (generated list)
if [ ! -f "$BUILD/spike_stubs.o" ]; then
  cp "$BOX/spike_stubs.c" "$BUILD/spike_stubs.c" 2>/dev/null || true
  $CC -c "$BOX/spike_stubs.c" -o "$BUILD/spike_stubs.o"
fi

# 3. ARC Objective-C: real Metal backend + windowed main
$ARCC -I"$WG/Sources/Core" -I"$WG/Sources/Graphics" -c "$WG/Sources/Graphics/WGMetalBackend.m" -o "$BUILD/WGMetalBackend.o"
$ARCC -I"$WG/Sources/Core" -c "$WG/Tests/mac_window_main.m" -o "$BUILD/mac_window_main.o"

# 4. engine core + LZMA + box64 backend + window -> wineglass_window
#    -pagezero_size 0x4000: shrink __PAGEZERO so box64's identity-mapped guest memory
#    can live where it needs (Apple Silicon still reserves the low 4GB, so the engine's
#    thunk base / image must be relocated above 4GB for a real boot — WIP).
#    -undefined dynamic_lookup: box64's unused Linux-layer externs resolve lazily.
$CC \
  -I"$WG/Sources/Core" -I"$WG/Sources/CPU" -I"$WG/Sources/PE" -I"$WG/Sources/Memory" \
  -I"$WG/Sources/Win32" -I"$WG/Sources/Graphics" -I"$WG/Sources/LZMA" \
  "$WG/Sources/Core/wg_engine.c" \
  "$WG/Sources/Core/wg_log.c" \
  "$WG/Sources/CPU/wg_x86_decode.c" "$WG/Sources/CPU/wg_x86_interp.c" "$WG/Sources/CPU/wg_x86_state.c" \
  "$WG/Sources/PE/wg_pe_loader.c" "$WG/Sources/Memory/wg_memory.c" \
  "$WG"/Sources/Win32/wg_dll_mapper.c "$WG"/Sources/Win32/wg_nsis_extract.c \
  "$WG"/Sources/Win32/wg_schannel.c "$WG"/Sources/Win32/wg_threading.c "$WG"/Sources/Win32/wg_sync.c \
  "$WG"/Sources/Win32/wg_win32_bitmap.c "$WG"/Sources/Win32/wg_win32_files.c \
  "$WG"/Sources/Win32/wg_win32_gdi.c "$WG"/Sources/Win32/wg_win32_windows.c \
  "$WG"/Sources/Win32/wg_winhttp.c "$WG"/Sources/Win32/wg_winsock.c \
  "$WG"/Sources/Win32/wg_d3d11.c "$WG"/Sources/Graphics/wg_dxbc.c \
  "$WG/Sources/LZMA/LzmaDec.c" \
  "$WG/Tests/wg_native_download_mac.m" \
  "$BUILD/WGMetalBackend.o" "$BUILD/mac_window_main.o" \
  "$BUILD/wg_blink_box64.o" "$LIBBOX" "$BUILD/spike_stubs.o" \
  -Wl,-pagezero_size,0x4000 -Wl,-undefined,dynamic_lookup \
  -framework Security -framework CoreFoundation -framework Foundation \
  -framework CoreGraphics -framework CoreText \
  -framework Cocoa -framework Metal -framework QuartzCore \
  -framework AVFoundation -framework CoreVideo -framework CoreMedia -framework CoreImage \
  -o "$BUILD/wineglass_window"
echo "built: $BUILD/wineglass_window (box64 backend)"
