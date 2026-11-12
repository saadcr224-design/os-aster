"""Thin wrapper around loginctl, the systemd-logind command line."""

from __future__ import annotations

import re
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass, field

SEAT_NAME = re.compile(r"^seat[A-Za-z0-9_-]+$")


class LogindError(RuntimeError):
    pass


def validate_seat(name: str) -> str:
    if not SEAT_NAME.match(name) or len(name) > 255:
        raise LogindError(
            f"invalid seat name {name!r}: must start with 'seat' and use only letters, digits, '-' and '_'"
        )
    return name


@dataclass
class Runner:
    """Runs (or, in dry-run mode, only prints) loginctl commands."""

    dry_run: bool = False
    executed: list[list[str]] = field(default_factory=list)

    def run(self, argv: list[str]) -> None:
        self.executed.append(argv)
        if self.dry_run:
            print("would run:", shlex.join(argv))
            return
        if shutil.which(argv[0]) is None:
            raise LogindError(f"{argv[0]} not found; openseat needs systemd-logind")
        print("+", shlex.join(argv), file=sys.stderr)
        proc = subprocess.run(argv, capture_output=True, text=True)
        if proc.returncode != 0:
            detail = proc.stderr.strip() or proc.stdout.strip() or f"exit code {proc.returncode}"
            raise LogindError(f"{shlex.join(argv)} failed: {detail}")

    def attach(self, seat: str, syspaths: list[str]) -> None:
        if syspaths:
            self.run(["loginctl", "attach", validate_seat(seat), *syspaths])

    def flush(self) -> None:
        self.run(["loginctl", "flush-devices"])


def capture(argv: list[str]) -> str:
    """Run a read-only command and return its output ('' if unavailable)."""
    if shutil.which(argv[0]) is None:
        return ""
    try:
        return subprocess.run(argv, capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""
