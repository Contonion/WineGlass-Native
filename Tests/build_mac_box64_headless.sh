#!/bin/bash
# build_mac_box64_headless.sh — Headless WineGlass engine on the box64 backend.
# Loads a .exe and ticks to completion, printing all logs to stdout (run_exe.c) —
# no Cocoa window (the windowed app SIGKILLs when run without a GUI session).
# Use this to trace a box64 boot on the mac harness.
#
#   Tests/build_mac_box64_headless.sh && \
#     WG_CPU=box64 /tmp/wineglass_box64h/wineglass_run <file.exe> [max_seconds]
#
# Make a minimal 64-bit test PE (mingw-w64):
#   x86_64-w64-mingw32-gcc -O2 -nostdlib -Wl,-e,mainCRTStartup -o mini64.exe mini64.c -lkernel32
#
# NOTE: do NOT add -pagezero_size here — combined with the framework link the kernel
# SIGKILLs the binary at exec. box64 is identity-mapped, so ALL guest memory must live
# above the 4 GB __PAGEZERO instead (see PRAGMATIC_REARCH.md "THE REMAINING BLOCKER").
set -e
WG="$(cd "$(dirname "$0")/.." && pwd)"
BOX="$WG/Vendor/box64"
BUILD="${BUILD_DIR:-/tmp/wineglass_box64h}"
mkdir -p "$BUILD"
SDK="$(xcrun --sdk macosx --show-sdk-path)"
LIBBOX="$BOX/build_mac/libbox64all.a"
[ -f "$LIBBOX" ] || { echo "ERROR: $LIBBOX missing — build box64 core first (see build_mac_window_box64.sh)"; exit 1; }

# box64 backend glue (box64 headers shadow system headers -> its own flags only)
clang -arch arm64 -O2 -g -Wno-everything -DARM64 -DDYNAREC -DM1 \
  -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -Dsincos=__sincos \
  -I"$BOX/src/dynarec/arm64" -I"$BOX/src/include" -I"$BOX/src" -I"$BOX/src/emu" -I"$BOX/src/wrapped/generated" \
  -c "$WG/Sources/Core/wg_blink_box64.c" -o "$BUILD/wg_blink_box64.o"
clang -arch arm64 -isysroot "$SDK" -O2 -g -Wno-everything -c "$BOX/spike_stubs.c" -o "$BUILD/spike_stubs.o"

clang -arch arm64 -isysroot "$SDK" -O2 -g -Wno-everything \
  -I"$WG/Sources/Core" -I"$WG/Sources/CPU" -I"$WG/Sources/PE" -I"$WG/Sources/Memory" \
  -I"$WG/Sources/Win32" -I"$WG/Sources/Graphics" -I"$WG/Sources/LZMA" \
  "$WG/Sources/Core/wg_engine.c" "$WG/Sources/Core/wg_log.c" \
  "$WG/Sources/CPU/wg_x86_decode.c" "$WG/Sources/CPU/wg_x86_interp.c" "$WG/Sources/CPU/wg_x86_state.c" \
  "$WG/Sources/PE/wg_pe_loader.c" "$WG/Sources/Memory/wg_memory.c" \
  "$WG"/Sources/Win32/wg_dll_mapper.c "$WG"/Sources/Win32/wg_nsis_extract.c \
  "$WG"/Sources/Win32/wg_schannel.c "$WG"/Sources/Win32/wg_threading.c "$WG"/Sources/Win32/wg_sync.c \
  "$WG"/Sources/Win32/wg_win32_bitmap.c "$WG"/Sources/Win32/wg_win32_files.c \
  "$WG"/Sources/Win32/wg_win32_gdi.c "$WG"/Sources/Win32/wg_win32_windows.c \
  "$WG"/Sources/Win32/wg_winhttp.c "$WG"/Sources/Win32/wg_winsock.c "$WG"/Sources/Win32/wg_d3d11.c \
  "$WG/Sources/LZMA/LzmaDec.c" "$WG/Tests/run_exe.c" \
  "$BUILD/wg_blink_box64.o" "$LIBBOX" "$BUILD/spike_stubs.o" \
  -Wl,-undefined,dynamic_lookup \
  -framework Security -framework CoreFoundation -framework Foundation -framework CoreGraphics -framework CoreText \
  -o "$BUILD/wineglass_run"
echo "built: $BUILD/wineglass_run (headless box64 backend)"
