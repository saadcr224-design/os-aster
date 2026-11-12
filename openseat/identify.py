"""Find out which keyboard or mouse the user is touching."""

from __future__ import annotations

import os
import select
import struct
import time
from collections.abc import Iterator
from pathlib import Path

from .devices import Device, event_nodes

INPUT_KINDS = ("keyboard", "mouse", "combo", "pointer")
_EVENT = struct.Struct("llHHi")  # struct input_event
_REAL_INPUT = {1, 2, 3}  # EV_KEY, EV_REL, EV_ABS


class IdentifyError(RuntimeError):
    pass


class Watcher:
    """Watches the evdev nodes of every keyboard and mouse."""

    def __init__(self, devices: list[Device], dev_input: str | Path = "/dev/input"):
        self._fds: dict[int, Device] = {}
        denied = 0
        for device in devices:
            if device.kind not in INPUT_KINDS:
                continue
            for name in event_nodes(device):
                try:
                    self._fds[os.open(Path(dev_input) / name, os.O_RDONLY | os.O_NONBLOCK)] = device
                except PermissionError:
                    denied += 1
                except OSError:
                    pass
        if not self._fds:
            raise IdentifyError(
                "permission denied reading input devices (run with sudo)"
                if denied
                else "no keyboards or mice found"
            )

    def __len__(self) -> int:
        return len(self._fds)

    def __enter__(self) -> "Watcher":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        for fd in self._fds:
            os.close(fd)
        self._fds = {}

    def events(self, timeout: float) -> Iterator[Device]:
        """Yield a device each time a different one produces input."""
        deadline = time.monotonic() + timeout
        last: str | None = None
        while (left := deadline - time.monotonic()) > 0:
            ready, _, _ = select.select(list(self._fds), [], [], left)
            for fd in ready:
                try:
                    data = os.read(fd, _EVENT.size * 64)
                except OSError:
                    continue
                types = {
                    _EVENT.unpack_from(data, offset)[2]
                    for offset in range(0, len(data) - _EVENT.size + 1, _EVENT.size)
                }
                device = self._fds[fd]
                if types & _REAL_INPUT and device.id != last:
                    last = device.id
                    yield device
