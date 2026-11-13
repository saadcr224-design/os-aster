#!/bin/sh
# Build VISION-Setup.exe. Needs MinGW-w64 and NSIS (apt install gcc-mingw-w64-x86-64 nsis).
set -eu
cd "$(dirname "$0")"
out="${1:-VISION-Setup.exe}"
../openseat/build.sh "$PWD/VISION.exe"
makensis -V2 vision.nsi
rm -f VISION.exe
[ "$out" = "VISION-Setup.exe" ] || mv VISION-Setup.exe "$out"
echo "built $out"
