# Windows multiseat: design notes

**Status: design only.** The only code here is `seatenum.c`, a user-mode tool
that lists monitors, keyboards and mice. It compiles with MinGW but has not
been run on Windows yet. Nothing in this folder creates a seat.

These notes are a starting plan, not a verified design. Items marked
*(unverified)* are assumptions that need a prototype to confirm.

## Why Windows is harder than Linux

Linux has multiseat built in: the kernel exposes each graphics card and input
device separately, and logind groups them into seats. Windows has no such
feature for local hardware:

- Only one session (the "console" session) owns the physical monitors,
  keyboards and mice.
- All keyboards feed one input queue and all mice move one cursor.
- Extra interactive sessions exist (Remote Desktop), but they render to a
  network client, not to a local monitor.

So a Windows multiseat product has to add what the OS lacks, in kernel mode.

## Components needed

| # | Component | Mode | Purpose | State |
| --- | --- | --- | --- | --- |
| 1 | Device discovery | user | List monitors/keyboards/mice, identify them | `seatenum.c` |
| 2 | Seat configuration | user | Store "this monitor + keyboard + mouse = seat N" | not started |
| 3 | Session service | user (service) | Start one extra interactive session per seat and keep it alive | not started |
| 4 | Input filter drivers | kernel | Send each keyboard/mouse only to its seat's session | not started |
| 5 | Display routing | kernel | Show each session on its own physical monitor | not started |
| 6 | Audio routing | user/kernel | Give each session its own sound device | not started |

### 4. Input filter drivers

Upper filter drivers on the keyboard and mouse class stacks, written with
KMDF. Microsoft's WDK ships sample filters (`kbfiltr`, `moufiltr`) that show
the attach point. The filter must take input from a seat's devices out of the
console session's stream and deliver it to that seat's session
*(unverified: the delivery mechanism into another session is the open
question)*.

### 5. Display routing

The hardest part. A non-console session has no physical display. Options to
prototype:

- An indirect display driver (IddCx) that gives each extra session a virtual
  monitor, plus a compositor that presents that image on the real monitor
  reserved for the seat *(unverified)*.
- Removing the reserved monitor from the console desktop so the main user's
  windows cannot land on it.

### Driver signing

64-bit Windows loads only signed kernel drivers. Development needs test
signing mode; shipping needs an EV code-signing certificate and Microsoft
attestation signing. This is a cost and process requirement, not a coding one.

### Licensing

Running several interactive sessions at once on a desktop edition of Windows
may conflict with the Windows licence terms. Check this before distributing
anything.

## Suggested order of work

1. Run `seatenum.exe` and `seatenum.exe --watch` on real hardware; fix issues.
2. Seat configuration file and a small GUI/CLI to edit it.
3. Prototype the session service on a test VM.
4. Input filter driver in a VM with test signing.
5. Display routing prototype.

## Building seatenum

```bash
x86_64-w64-mingw32-gcc -O2 -Wall -municode seatenum.c -o seatenum.exe -luser32
```

With Visual Studio: `cl /W4 /DUNICODE /D_UNICODE seatenum.c user32.lib`
