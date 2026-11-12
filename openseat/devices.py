"""Discover seat-assignable hardware from sysfs.

logind can only move devices that udev tags "seat": graphics cards, input
devices, sound cards and USB hubs.  This module finds those nodes and groups
them into physical devices (one USB keyboard usually shows up as several input
nodes), so the rest of the tool can talk about "the keyboard on port 1-4".
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_SEAT = "seat0"

PCI_ADDR = re.compile(r"^[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]$")
CLASS_DIRS = {"input", "drm", "sound", "graphics"}

# Linux input event codes (include/uapi/linux/input-event-codes.h)
EV_KEY, EV_REL, EV_ABS = 0x01, 0x02, 0x03
REL_X, REL_Y = 0x00, 0x01
KEY_Q, KEY_P = 16, 25
BTN_LEFT, BTN_TOUCH = 0x110, 0x14A

KIND_ORDER = ["gpu", "keyboard", "mouse", "combo", "pointer", "sound", "usb-hub", "other"]


@dataclass(frozen=True)
class Roots:
    """Filesystem locations, overridable so tests can use a fake tree."""

    sys: Path = Path("/sys")
    udev_data: Path = Path("/run/udev/data")

    @classmethod
    def from_env(cls) -> "Roots":
        return cls(
            sys=Path(os.environ.get("OPENSEAT_SYSFS", "/sys")),
            udev_data=Path(os.environ.get("OPENSEAT_UDEV_DATA", "/run/udev/data")),
        )


@dataclass
class Node:
    """One sysfs device that logind can attach to a seat."""

    syspath: Path
    subsystem: str
    seat: str
    role: str  # gpu, fb, keyboard, mouse, pointer, sound, usb-hub, other
    label: str = ""


@dataclass
class Device:
    """A physical device: one or more nodes that should move together."""

    id: str
    kind: str
    name: str
    nodes: list[Node] = field(default_factory=list)
    hub: str | None = None  # id of the external USB hub it is plugged into
    details: list[str] = field(default_factory=list)

    @property
    def seat(self) -> str:
        seats = {n.seat for n in self.nodes}
        return seats.pop() if len(seats) == 1 else "mixed"

    @property
    def syspaths(self) -> list[str]:
        return [str(n.syspath) for n in self.nodes]


def _read(path: Path) -> str:
    try:
        return path.read_text(errors="replace").strip()
    except OSError:
        return ""


def _subsystem(syspath: Path) -> str:
    try:
        return os.path.basename(os.readlink(syspath / "subsystem"))
    except OSError:
        return ""


def _udev_id(syspath: Path, subsystem: str) -> str:
    dev = _read(syspath / "dev")
    if dev:
        return f"c{dev}"
    return f"+{subsystem}:{syspath.name}"


def seat_of(syspath: Path, subsystem: str, roots: Roots) -> str:
    """Read the seat udev recorded for a device (seat0 when unset)."""
    data = roots.udev_data / _udev_id(syspath, subsystem)
    try:
        for line in data.read_text(errors="replace").splitlines():
            if line.startswith("E:ID_SEAT="):
                return line.split("=", 1)[1].strip() or DEFAULT_SEAT
    except OSError:
        pass
    return DEFAULT_SEAT


def parse_bitmap(text: str, word_bits: int = 64) -> int:
    """Turn a sysfs capability bitmap ("3 10000 ...", high word first) into an int."""
    value = 0
    for word in text.split():
        value = (value << word_bits) | int(word, 16)
    return value


def classify_input(syspath: Path) -> str:
    caps = syspath / "capabilities"
    ev = parse_bitmap(_read(caps / "ev") or "0")
    key = parse_bitmap(_read(caps / "key") or "0")
    rel = parse_bitmap(_read(caps / "rel") or "0")

    def has(bitmap: int, bit: int) -> bool:
        return bool(bitmap >> bit & 1)

    if has(ev, EV_KEY) and all(has(key, k) for k in range(KEY_Q, KEY_P + 1)):
        return "keyboard"
    if has(ev, EV_REL) and has(rel, REL_X) and has(rel, REL_Y) and has(key, BTN_LEFT):
        return "mouse"
    if has(ev, EV_ABS) and (has(key, BTN_TOUCH) or has(key, BTN_LEFT)):
        return "pointer"
    return "other"


def _is_usb_device(path: Path) -> bool:
    return (path / "busnum").exists() and (path / "devnum").exists()


def _is_hub(path: Path) -> bool:
    return _read(path / "bDeviceClass") == "09"


def _is_root_hub(path: Path) -> bool:
    return re.fullmatch(r"usb\d+", path.name) is not None


def _usb_name(path: Path) -> str:
    name = " ".join(x for x in (_read(path / "manufacturer"), _read(path / "product")) if x)
    if not name:
        vid, pid = _read(path / "idVendor"), _read(path / "idProduct")
        name = f"USB device {vid}:{pid}" if vid else "USB device"
    return name


def _pci_name(path: Path) -> str:
    vendor, device = _read(path / "vendor"), _read(path / "device")
    if vendor and device:
        return f"PCI {vendor.removeprefix('0x')}:{device.removeprefix('0x')}"
    return "PCI device"


def _anchor(node: Path, devices_root: Path) -> tuple[str, Path]:
    """Find the physical device a node belongs to: (id, sysfs path)."""
    fallback: Path | None = None
    for parent in node.parents:
        if parent == devices_root or devices_root not in parent.parents:
            break
        if _is_usb_device(parent) and not _is_hub(parent):
            return f"usb:{parent.name}", parent
        if PCI_ADDR.match(parent.name):
            return f"pci:{parent.name}", parent
        if fallback is None and parent.name not in CLASS_DIRS:
            fallback = parent
    fallback = fallback or node
    return f"sys:{fallback.name}", fallback


def _external_hub(anchor: Path, devices_root: Path) -> str | None:
    for parent in anchor.parents:
        if parent == devices_root:
            break
        if _is_usb_device(parent) and _is_hub(parent) and not _is_root_hub(parent):
            return f"hub:{parent.name}"
    return None


def _class_nodes(roots: Roots, cls: str, pattern: str) -> list[Path]:
    base = roots.sys / "class" / cls
    if not base.is_dir():
        return []
    rx = re.compile(pattern)
    found = [p.resolve() for p in base.iterdir() if rx.fullmatch(p.name)]
    return sorted(found, key=lambda p: _natural(p.name))


def _natural(text: str) -> list:
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", text)]


def _connectors(card: Path) -> list[str]:
    out = []
    for conn in sorted(card.glob(f"{card.name}-*"), key=lambda p: _natural(p.name)):
        status = _read(conn / "status") or "unknown"
        out.append(f"{conn.name.removeprefix(card.name + '-')}: {status}")
    return out


def _group_kind(nodes: list[Node]) -> str:
    roles = {n.role for n in nodes}
    if "gpu" in roles:
        return "gpu"
    if {"keyboard", "mouse"} <= roles:
        return "combo"
    for role in ("keyboard", "mouse", "pointer", "sound"):
        if role in roles:
            return role
    return "other"


def discover(roots: Roots | None = None) -> list[Device]:
    """Return every seat-assignable physical device, plus external USB hubs."""
    roots = roots or Roots.from_env()
    devices_root = (roots.sys / "devices").resolve()

    found: list[tuple[Path, str, str, str, list[str]]] = []  # path, subsystem, role, label, details
    for card in _class_nodes(roots, "drm", r"card\d+"):
        found.append((card, "drm", "gpu", card.name, _connectors(card)))
    for fb in _class_nodes(roots, "graphics", r"fb\d+"):
        found.append((fb, "graphics", "fb", fb.name, []))
    for inp in _class_nodes(roots, "input", r"input\d+"):
        found.append((inp, "input", classify_input(inp), _read(inp / "name") or inp.name, []))
    for snd in _class_nodes(roots, "sound", r"card\d+"):
        found.append((snd, "sound", "sound", _read(snd / "id") or snd.name, []))

    groups: dict[str, Device] = {}
    for path, subsystem, role, label, details in found:
        dev_id, anchor = _anchor(path, devices_root)
        device = groups.get(dev_id)
        if device is None:
            if dev_id.startswith("usb:"):
                name = _usb_name(anchor)
            elif dev_id.startswith("pci:"):
                name = _pci_name(anchor)
            else:
                name = label
            device = groups[dev_id] = Device(
                id=dev_id, kind="other", name=name, hub=_external_hub(anchor, devices_root)
            )
        device.nodes.append(
            Node(syspath=path, subsystem=subsystem, seat=seat_of(path, subsystem, roots), role=role, label=label)
        )
        device.details.extend(details)

    for device in groups.values():
        device.kind = _group_kind(device.nodes)
        if not device.id.startswith("usb:"):
            labels = [n.label for n in device.nodes if n.role == device.kind]
            if labels and device.kind in ("keyboard", "mouse", "pointer", "sound"):
                device.name = labels[0]

    result = list(groups.values())

    usb_bus = roots.sys / "bus" / "usb" / "devices"
    if usb_bus.is_dir():
        for entry in sorted(usb_bus.iterdir(), key=lambda p: _natural(p.name)):
            path = entry.resolve()
            if ":" in entry.name or not _is_hub(path) or _is_root_hub(path):
                continue
            result.append(
                Device(
                    id=f"hub:{path.name}",
                    kind="usb-hub",
                    name=_usb_name(path),
                    nodes=[Node(path, "usb", seat_of(path, "usb", roots), "usb-hub", path.name)],
                    hub=_external_hub(path, devices_root),
                )
            )

    result.sort(key=lambda d: (KIND_ORDER.index(d.kind), _natural(d.id)))
    return result


def is_boot_gpu(device: Device) -> bool:
    """True for the graphics card the firmware booted on (keep it on seat0)."""
    for node in device.nodes:
        for parent in node.syspath.parents:
            if _read(parent / "boot_vga") == "1":
                return True
    return False


def event_nodes(device: Device) -> list[str]:
    """Names of the /dev/input/eventN nodes that belong to a device."""
    names = []
    for node in device.nodes:
        if node.subsystem == "input":
            names += sorted(p.name for p in node.syspath.glob("event*"))
    return names
