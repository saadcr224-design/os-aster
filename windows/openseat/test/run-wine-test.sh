#!/bin/sh
# End-to-end test of OpenSeat's input routing under Wine, with a stand-in for
# the Interception driver.  Needs: wine, Xvfb, MinGW-w64.
# Checks that workplace 1's devices pass through untouched, that workplace 2's
# keyboard reaches the agent, and that an unknown mouse can be picked by
# clicking it and is then routed to workplace 2 only.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'wineserver -k 2>/dev/null || true; kill $xvfb 2>/dev/null || true; rm -rf "$work"' EXIT
export WINEPREFIX="$work/prefix" WINEDEBUG=-all DISPLAY=:93
export OPENSEAT_TEST_SCREENS="0,0,1280,900;1280,0,2304,768"
Xvfb :93 -screen 0 2304x900x24 >/dev/null 2>&1 & xvfb=$!
sleep 1
"$here/../build.sh" "$work/OpenSeat.exe" >/dev/null
x86_64-w64-mingw32-gcc -shared -O2 "$here/fakeintercept.c" -o "$work/interception.dll"
wineboot -i >/dev/null 2>&1 || true

dir="$WINEPREFIX/drive_c/users/$(whoami)/AppData/Roaming/OpenSeat"
mkdir -p "$dir"
printf '%s\r\n' '[Setup]' 'Workplaces=2' '[Screens]' 'ktest-screen-1=1' 'ktest-screen-2=2' \
  '[Devices]' 'kfake-kb-1=1' 'kfake-kb-2=2' 'kfake-mouse-1=1' \
  '[Hardware]' 'kfake-kb-1=HID\VID_1111&PID_0001' 'kfake-kb-2=HID\VID_2222&PID_0002' 'kfake-mouse-1=HID\VID_1111&PID_0011' \
  > "$dir/setup.ini"
cat > "$work/script.txt" <<'S'
hw 1 HID\VID_1111&PID_0001
hw 2 HID\VID_2222&PID_0002
hw 11 HID\VID_1111&PID_0011
hw 12 HID\VID_3333&PID_0012
sleep 6000
k 1 30 0
k 1 30 1
k 2 31 0
k 2 31 1
k 2 75 3
m 11 0 0 0 4 4
m 12 0 0 0 7 7
m 12 1 0 0 0 0
sleep 500
m 12 0 0 0 5 -3
m 12 1 0 0 0 0
m 12 2 0 0 0 0
m 12 1024 0 120 0 0
S
cd "$work"
OPENSEAT_FAKE_SCRIPT="Z:$(echo "$work/script.txt" | tr / '\\')" OPENSEAT_FAKE_LOG="$work/fake.log" \
  OPENSEAT_TEST_SEAT=2 wine OpenSeat.exe --start --test >/dev/null 2>&1 &
sleep 2
OPENSEAT_TEST_AGENT_LOG="$work/agent.log" wine OpenSeat.exe --agent --test >/dev/null 2>&1 &
sleep 14

cat > "$work/expect-fake.log" <<'E'
send key dev=1 code=30 state=0
send key dev=1 code=30 state=1
send mouse dev=11 state=0 x=4 y=4
send mouse dev=12 state=0 x=7 y=7
E
cat > "$work/expect-agent.log" <<'E'
key scan=31 flags=8
key scan=31 flags=10
key scan=75 flags=11
mouse dx=5 dy=-3 flags=1 data=0
mouse dx=0 dy=0 flags=2 data=0
mouse dx=0 dy=0 flags=4 data=0
mouse dx=0 dy=0 flags=2048 data=120
E
ok=0
tr -d "\r" < "$work/fake.log" > "$work/fake.txt"; diff -u "$work/expect-fake.log" "$work/fake.txt" || ok=1
tr -d "\r" < "$work/agent.log" > "$work/agent.txt"; diff -u "$work/expect-agent.log" "$work/agent.txt" || ok=1
[ $ok = 0 ] && echo "PASS: input routing" || echo "FAIL: input routing"
exit $ok
