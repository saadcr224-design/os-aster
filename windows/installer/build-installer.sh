#!/bin/sh
# Build VISION-Setup.exe. Needs MinGW-w64 and NSIS (apt install gcc-mingw-w64-x86-64 nsis).
set -eu
cd "$(dirname "$0")"
out="${1:-VISION-Setup.exe}"
../openseat/build.sh "$PWD/VISION.exe"
ver=$(sed -n 's/^#define VISION_VERSION "\(.*\)"/\1/p' ../openseat/version.h)
makensis -V2 -DVERSION="$ver" -DVERSION4="$ver.0" vision.nsi
rm -f VISION.exe
[ "$out" = "VISION-Setup.exe" ] || mv VISION-Setup.exe "$out"
echo "built $out"
