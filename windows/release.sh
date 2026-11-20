#!/bin/sh
# Build VISION and publish it in docs/download, including version.txt, which
# installed copies of VISION read to offer the update.
#   windows/release.sh "One line about what is new"
# Raise VISION_VERSION (and VISION_VERSION_RC) in windows/openseat/version.h first.
set -eu
cd "$(dirname "$0")/.."
notes="${1:-}"
ver=$(sed -n 's/^#define VISION_VERSION "\(.*\)"/\1/p' windows/openseat/version.h)
windows/installer/build-installer.sh >/dev/null
mv -f windows/installer/VISION-Setup.exe docs/download/VISION-Setup.exe
windows/openseat/build.sh "$PWD/docs/download/VISION.exe" >/dev/null
(cd docs/download && rm -f VISION-Setup.zip && python3 -c "import zipfile;z=zipfile.ZipFile('VISION-Setup.zip','w',zipfile.ZIP_DEFLATED);z.write('VISION-Setup.exe');z.close()")
python3 - <<PY
import zipfile
z=zipfile.ZipFile('docs/download/VISION-Server-USB-kit.zip','w',zipfile.ZIP_DEFLATED)
z.write('windows/server-usb/autounattend.xml','autounattend.xml'); z.write('windows/server-usb/README.txt','README.txt')
z.write('docs/download/VISION-Setup.exe','VISION/VISION-Setup.exe'); z.close()
PY
sum=$(sha256sum docs/download/VISION-Setup.exe | cut -d' ' -f1)
printf 'version=%s\nfile=VISION-Setup.exe\nsha256=%s\nnotes=%s\n' "$ver" "$sum" "$notes" > docs/download/version.txt
echo "released VISION $ver"
cat docs/download/version.txt
