#!/bin/sh
# Build OpenSeat.exe with MinGW-w64 (on Linux: apt install gcc-mingw-w64-x86-64; on Windows: MSYS2).
set -eu
cd "$(dirname "$0")"
out="${1:-OpenSeat.exe}"
x86_64-w64-mingw32-windres app.rc -O coff -o app.res
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -municode -mwindows -static main.c common.c run.c agent.c app.res -o "$out" \
    -luser32 -lgdi32 -lcomctl32 -lhid -lcfgmgr32 -lshell32 -ladvapi32 -lwtsapi32 -lnetapi32 -lbcrypt
rm -f app.res
echo "built $out"
