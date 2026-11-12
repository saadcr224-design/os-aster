# os-aster / openseat

Open-source multiseat: several people use **one PC** at the same time, each
with their own monitor, keyboard, mouse and sound.

This is an original project written from scratch. It is not affiliated with,
and contains no code from, ASTER or IBIK.

| Part | Status |
| --- | --- |
| **Linux** (`openseat`) | Working command-line tool. Tested against simulated hardware; not yet run on a real multi-GPU PC. |
| **Windows** (`windows/`) | Device-discovery tool only (compiles, not yet run on Windows). Creating seats needs kernel drivers that are **not written** — see [windows/DESIGN.md](windows/DESIGN.md). |

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

### Use

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

See [windows/DESIGN.md](windows/DESIGN.md). In short: `windows/seatenum.c`
lists monitors, keyboards and mice and can show which input device is in use,
but real multiseat on Windows needs signed kernel drivers that do not exist
in this repository yet.

## License

MIT
