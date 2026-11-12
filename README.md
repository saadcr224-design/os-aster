# os-aster / openseat

Open-source multiseat: several people use **one PC** at the same time, each
with their own monitor, keyboard, mouse and sound.

This is an original project written from scratch. It is not affiliated with,
and contains no code from, ASTER or IBIK.

| Part | Status |
| --- | --- |
| **Linux** (`openseat`) | Working command-line tool and graphical interface. Tested against simulated hardware; not yet run on a real multi-GPU PC. |
| **Windows** (`windows/openseat/`) | `OpenSeat.exe`: plan and save who uses which screen, keyboard and mouse. Tested under Wine, not yet on real Windows. It cannot start the extra workplaces: that needs kernel drivers that are **not written** — see [windows/DESIGN.md](windows/DESIGN.md). |

## Linux

Linux already supports multiseat through systemd-logind. `openseat` makes it
easy: it finds your hardware, suggests a split, and applies it.

### What you need

- A Linux system with systemd, and Python 3.11 or newer.
- **One graphics card per seat.** Linux cannot split two monitors on the same
  card between two users. A second card (PCI or USB) is enough.
- A keyboard and mouse per seat.
- A display manager with multiseat support (GDM and LightDM are known to work).

### Install

```bash
pip install .
```

### Graphical interface

```bash
openseat gui
```

This opens the seat manager in your web browser. Drag devices between seats
(or use the "Seat" menu on each device), then press **Review and apply**.

![openseat GUI](docs/gui.png)

- **Identify devices**: press a key or move a mouse and the device lights up.
  Needs permission to read input devices (`sudo openseat gui`, or be in the
  `input` group).
- **Auto arrange**: suggests one seat per graphics card.
- **Import / Export plan**: load or save the layout as a plan file.
- **Reset all**: every device back to `seat0`.

Nothing changes on the PC until you apply. When you apply, your system may
ask for an administrator password.

To try the interface without the right hardware (it works on any computer
with Python, including Windows and macOS):

```bash
openseat gui --demo
```

The GUI is a small local server. It listens only on this computer
(`127.0.0.1`) and only accepts requests carrying the secret code in the
address it prints, so keep that address private while it is running.

### Command line

See your hardware:

```bash
openseat list
```

```
SEAT   KIND      ID                NAME
seat0  gpu       pci:0000:00:02.0  PCI 8086:3e92
                                     HDMI-A-1: connected
seat0  gpu       pci:0000:01:00.0  PCI 10de:1c82
seat0  keyboard  usb:1-1           Acme Keyboard
seat0  keyboard  usb:1-3.1         Second Keyboard  (on hub:1-3)
seat0  usb-hub   hub:1-3           4-Port Hub
```

Find out which keyboard or mouse is which (press a key on it):

```bash
sudo openseat identify
```

Get a suggested plan, then edit it if needed:

```bash
openseat auto > seats.toml
```

Preview and apply it:

```bash
sudo openseat apply --dry-run seats.toml
```

```bash
sudo openseat apply seats.toml
```

A login screen appears on each seat's monitor. The assignment is permanent
(it survives reboots) until you change it.

### All commands

| Command | What it does |
| --- | --- |
| `openseat list [--all] [--json]` | List graphics cards, keyboards, mice, sound cards, USB hubs |
| `openseat seats` | Show each seat and its devices |
| `openseat identify` | Show which keyboard/mouse you are touching |
| `openseat auto` | Print a suggested seat plan |
| `openseat apply PLAN [--reset-others] [--strict]` | Apply a plan file |
| `openseat attach SEAT ID...` | Assign devices to a seat by hand |
| `openseat detach ID...` | Return devices to `seat0` |
| `openseat reset` | Undo everything: all devices back to `seat0` |
| `openseat doctor` | Check the PC is ready for multiseat |
| `openseat gui [--demo] [--port N] [--no-browser]` | Open the graphical interface |

Commands that change things accept `-n` / `--dry-run`.

### Plan file

```toml
[seats.seat0]
devices = ["pci:0000:00:02.0", "usb:1-1", "usb:1-2"]

[seats.seat1]
devices = ["pci:0000:01:00.0", "hub:1-3"]
```

- Seat names must start with `seat`.
- Assigning a USB hub (`hub:...`) moves everything plugged into it, including
  devices plugged in later. This is the easiest way to build a workstation.
- USB ids are port-based: `usb:1-3.1` means "whatever is in that port".

### Tests

```bash
python3 -m unittest
```

## Windows

`windows/openseat/` is a native Windows program, **OpenSeat.exe**. It shows
your screens, keyboards and mice, and lets you divide them into workplaces
(one workplace = one person):

![OpenSeat for Windows](docs/windows.png)

1. Press **Add a workplace**.
2. Click a screen, a keyboard and a mouse and choose the workplace.
3. Not sure which keyboard is which? Press **Find a device**, then press a
   key on it: it lights up in the list.
4. Press **Save setup**.

**Important:** this is a preview. It plans and saves the setup, but it cannot
start the extra workplaces yet. That needs kernel drivers that are not
written; see [windows/DESIGN.md](windows/DESIGN.md).

Run `OpenSeat.exe --demo` to try it with example hardware.

Build it yourself (needs MinGW-w64):

```bash
windows/openseat/build.sh
```

It has been tested under Wine on Linux, not yet on a real Windows PC. The
exe is not code-signed, so Windows SmartScreen will show a warning the first
time you run it.

## License

MIT
