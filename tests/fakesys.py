"""Build a fake /sys and udev database that looks like a two-seat PC."""

from __future__ import annotations

import os
from pathlib import Path

from openseat.devices import Roots

KEYBOARD = {"ev": "120013", "key": "fffffffffffffffe", "rel": "0"}
MOUSE = {"ev": "17", "key": "70000 0 0 0 0", "rel": "103"}
BUTTONS = {"ev": "3", "key": "10000000000000 0", "rel": "0"}


class FakeSys:
    def __init__(self, root: Path):
        self.sys = root / "sys"
        self.udev = root / "udev"
        self.dev_input = root / "dev-input"
        for d in (self.sys / "devices", self.udev, self.dev_input):
            d.mkdir(parents=True)
        self.roots = Roots(sys=self.sys, udev_data=self.udev)
        self._input = 0
        self._sound = 0

    def _dir(self, rel: str, /, **attrs: str) -> Path:
        path = self.sys / "devices" / rel
        path.mkdir(parents=True, exist_ok=True)
        for name, value in attrs.items():
            (path / name).write_text(value + "\n")
        return path

    def _register(self, path: Path, cls: str) -> None:
        class_dir = self.sys / "class" / cls
        class_dir.mkdir(parents=True, exist_ok=True)
        os.symlink(path, class_dir / path.name)
        os.symlink(class_dir, path / "subsystem")

    def pci(self, addr: str, **attrs: str) -> str:
        rel = f"pci0000:00/{addr}"
        self._dir(rel, **attrs)
        return rel

    def gpu(self, addr: str, index: int, boot: bool = False, connectors: dict[str, str] | None = None) -> Path:
        rel = self.pci(addr, boot_vga="1" if boot else "0", vendor="0x10de", device=f"0x000{index}")
        card = self._dir(f"{rel}/drm/card{index}", dev=f"226:{index}")
        self._register(card, "drm")
        for name, status in (connectors or {}).items():
            self._dir(f"{rel}/drm/card{index}/card{index}-{name}", status=status)
        return card

    def usb(self, rel: str, hub: bool = False, **attrs: str) -> str:
        path = self._dir(rel, busnum="1", devnum="2", bDeviceClass="09" if hub else "00", **attrs)
        bus = self.sys / "bus" / "usb" / "devices"
        bus.mkdir(parents=True, exist_ok=True)
        os.symlink(path, bus / path.name)
        if hub:
            os.symlink(bus, path / "subsystem")
        return rel

    def input(self, parent_rel: str, name: str, caps: dict[str, str]) -> Path:
        index = self._input
        self._input += 1
        node = self._dir(f"{parent_rel}/input/input{index}", name=name)
        self._dir(f"{parent_rel}/input/input{index}/capabilities", **caps)
        self._dir(f"{parent_rel}/input/input{index}/event{index}")
        self._register(node, "input")
        return node

    def sound(self, parent_rel: str, card_id: str) -> Path:
        index = self._sound
        self._sound += 1
        node = self._dir(f"{parent_rel}/sound/card{index}", id=card_id)
        self._register(node, "sound")
        return node

    def set_seat(self, udev_id: str, seat: str) -> None:
        (self.udev / udev_id).write_text(f"E:ID_SEAT={seat}\nG:seat\n")


def two_seat_pc(root: Path) -> FakeSys:
    """Two graphics cards, loose keyboard+mouse, and a hub with a second set."""
    fs = FakeSys(root)
    fs.gpu("0000:00:02.0", 0, boot=True, connectors={"HDMI-A-1": "connected", "DP-1": "disconnected"})
    fs.gpu("0000:01:00.0", 1, connectors={"HDMI-A-1": "connected"})

    xhci = fs.pci("0000:00:14.0")
    root_hub = fs.usb(f"{xhci}/usb1", hub=True)
    kb = fs.usb(f"{root_hub}/1-1", manufacturer="Acme", product="Keyboard")
    fs.input(f"{kb}/1-1:1.0/0003:0001:0001.0001", "Acme Keyboard", KEYBOARD)
    fs.input(f"{kb}/1-1:1.1/0003:0001:0001.0002", "Acme Keyboard Consumer Control", BUTTONS)
    mouse = fs.usb(f"{root_hub}/1-2", manufacturer="Acme", product="Mouse")
    fs.input(f"{mouse}/1-2:1.0/0003:0002:0002.0003", "Acme Mouse", MOUSE)

    hub = fs.usb(f"{root_hub}/1-3", hub=True, product="4-Port Hub")
    kb2 = fs.usb(f"{hub}/1-3.1", product="Second Keyboard")
    fs.input(f"{kb2}/1-3.1:1.0/0003:0003:0003.0004", "Second Keyboard", KEYBOARD)
    mouse2 = fs.usb(f"{hub}/1-3.2", product="Second Mouse")
    fs.input(f"{mouse2}/1-3.2:1.0/0003:0004:0004.0005", "Second Mouse", MOUSE)

    fs.sound(fs.pci("0000:00:1f.3", vendor="0x8086", device="0xa348"), "PCH")
    headset = fs.usb(f"{root_hub}/1-4", product="USB Headset")
    fs.sound(f"{headset}/1-4:1.0", "Headset")

    fs.input("LNXSYSTM:00/LNXPWRBN:00", "Power Button", BUTTONS)
    return fs
