"""What the GUI talks to: the real machine, or a simulated one for demos."""

from __future__ import annotations

import itertools
import shlex
import time
from pathlib import Path

from .devices import DEFAULT_SEAT, Device, Node, discover
from .identify import INPUT_KINDS, IdentifyError, Watcher
from .logind import Runner, validate_seat


class RealBackend:
    demo = False

    def devices(self) -> list[Device]:
        return discover()

    def attach(self, seat: str, device: Device) -> str:
        """Move a device; returns the command that was run."""
        Runner().attach(seat, device.syspaths)
        return shlex.join(["loginctl", "attach", seat, *device.syspaths])

    def reset(self) -> None:
        Runner().flush()

    def identify(self, timeout: float) -> str | None:
        with Watcher(self.devices()) as watcher:
            for device in watcher.events(timeout):
                return device.id
        return None


def _demo_device(dev_id: str, kind: str, name: str, hub: str | None = None, details=()) -> Device:
    node = Node(Path("/sys/devices/demo") / dev_id.replace(":", "_"), "demo", DEFAULT_SEAT, kind)
    return Device(id=dev_id, kind=kind, name=name, nodes=[node], hub=hub, details=list(details))


class DemoBackend:
    """A pretend PC with two graphics cards. Nothing on the real machine changes."""

    demo = True

    def __init__(self) -> None:
        self._devices = [
            _demo_device("pci:0000:00:02.0", "gpu", "Intel UHD Graphics 630", details=["HDMI-A-1: connected", "DP-1: disconnected"]),
            _demo_device("pci:0000:01:00.0", "gpu", "NVIDIA GeForce GT 1030", details=["HDMI-A-2: connected", "DVI-D-1: connected"]),
            _demo_device("usb:1-1", "keyboard", "Logitech K120 Keyboard"),
            _demo_device("usb:1-3.1", "keyboard", "Dell KB216 Keyboard", hub="hub:1-3"),
            _demo_device("usb:1-2", "mouse", "Logitech M100 Mouse"),
            _demo_device("usb:1-3.2", "mouse", "Dell MS116 Mouse", hub="hub:1-3"),
            _demo_device("pci:0000:00:1f.3", "sound", "Built-in Audio"),
            _demo_device("usb:1-4", "sound", "USB Headset"),
            _demo_device("hub:1-3", "usb-hub", "Generic 4-Port USB Hub"),
        ]
        inputs = [d.id for d in self._devices if d.kind in INPUT_KINDS]
        self._touch = itertools.cycle(inputs)

    def devices(self) -> list[Device]:
        return self._devices

    def _set(self, device: Device, seat: str) -> None:
        for node in device.nodes:
            node.seat = seat

    def attach(self, seat: str, device: Device) -> str:
        validate_seat(seat)
        self._set(device, seat)
        if device.kind == "usb-hub":  # like the real thing, a hub carries its devices along
            for child in self._devices:
                if child.hub == device.id:
                    self._set(child, seat)
        return f"(demo) loginctl attach {seat} {device.id}"

    def reset(self) -> None:
        for device in self._devices:
            self._set(device, DEFAULT_SEAT)

    def identify(self, timeout: float) -> str | None:
        time.sleep(min(timeout, 1.5))
        return next(self._touch)


__all__ = ["DemoBackend", "IdentifyError", "RealBackend"]
