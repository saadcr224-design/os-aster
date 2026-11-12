"""Seat plans: which device belongs to which seat."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from .devices import DEFAULT_SEAT, Device, is_boot_gpu
from .logind import validate_seat


class PlanError(ValueError):
    pass


@dataclass
class Plan:
    seats: dict[str, list[str]] = field(default_factory=dict)  # seat -> device ids

    def assignments(self) -> dict[str, str]:
        """device id -> seat, rejecting a device listed under two seats."""
        out: dict[str, str] = {}
        for seat, ids in self.seats.items():
            for dev_id in ids:
                if out.get(dev_id, seat) != seat:
                    raise PlanError(f"{dev_id} is listed under both {out[dev_id]} and {seat}")
                out[dev_id] = seat
        return out

    def to_toml(self, devices: list[Device] | None = None) -> str:
        names = {d.id: f"{d.kind}: {d.name}" for d in devices or []}
        lines = ["# openseat seat plan. Apply with: sudo openseat apply <this file>", ""]
        for seat, ids in self.seats.items():
            lines.append(f"[seats.{seat}]")
            lines.append("devices = [")
            for dev_id in ids:
                comment = f"  # {names[dev_id]}" if dev_id in names else ""
                lines.append(f'    "{dev_id}",{comment}')
            lines += ["]", ""]
        return "\n".join(lines)


def parse_plan(text: str) -> Plan:
    try:
        data = tomllib.loads(text)
    except tomllib.TOMLDecodeError as exc:
        raise PlanError(f"invalid TOML: {exc}") from exc
    seats = data.get("seats")
    if not isinstance(seats, dict) or not seats:
        raise PlanError("plan needs at least one [seats.<name>] table")
    plan = Plan()
    for seat, body in seats.items():
        try:
            validate_seat(seat)
        except RuntimeError as exc:
            raise PlanError(str(exc)) from exc
        ids = body.get("devices") if isinstance(body, dict) else None
        if not isinstance(ids, list) or not all(isinstance(i, str) for i in ids):
            raise PlanError(f"[seats.{seat}] needs devices = [\"id\", ...]")
        plan.seats[seat] = ids
    plan.assignments()
    return plan


def load_plan(path: str | Path) -> Plan:
    try:
        return parse_plan(Path(path).read_text())
    except OSError as exc:
        raise PlanError(f"cannot read {path}: {exc.strerror}") from exc


@dataclass
class Action:
    seat: str
    device: Device


@dataclass
class Diff:
    actions: list[Action] = field(default_factory=list)
    unchanged: list[Device] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)  # ids in the plan but not plugged in


def diff(plan: Plan, devices: list[Device], reset_others: bool = False) -> Diff:
    """Work out which devices must move to match the plan.

    Devices the plan does not mention are left alone unless reset_others is
    set, in which case they go back to seat0.
    """
    by_id = {d.id: d for d in devices}
    wanted = plan.assignments()
    result = Diff(missing=[i for i in wanted if i not in by_id])

    # A device behind a hub that the plan moves follows the hub automatically.
    hub_seat = {i: s for i, s in wanted.items() if i.startswith("hub:")}

    for device in devices:
        target = wanted.get(device.id)
        if target is None:
            if device.hub in hub_seat or not reset_others:
                continue
            target = DEFAULT_SEAT
        if device.seat == target:
            result.unchanged.append(device)
        else:
            result.actions.append(Action(target, device))
    # Hubs first, so explicit per-device choices made afterwards win.
    result.actions.sort(key=lambda a: a.device.kind != "usb-hub")
    return result


def auto_plan(devices: list[Device]) -> Plan:
    """Suggest a plan: one seat per graphics card, inputs dealt out in turn.

    The firmware's boot graphics card stays on seat0.  This is only a starting
    point: use `openseat identify` to find out which keyboard is which.
    """
    gpus = [d for d in devices if d.kind == "gpu"]
    gpus.sort(key=lambda d: not is_boot_gpu(d))
    seat_names = [DEFAULT_SEAT] + [f"seat{i}" for i in range(1, len(gpus))]
    plan = Plan({name: [] for name in seat_names})
    if not gpus:
        return plan
    for seat, gpu in zip(seat_names, gpus):
        plan.seats[seat].append(gpu.id)

    hubs_with_inputs = {d.hub for d in devices if d.hub and d.kind in ("keyboard", "mouse", "combo")}
    stations = [d for d in devices if d.kind == "usb-hub" and d.id in hubs_with_inputs]
    loose = [d for d in devices if d.hub not in hubs_with_inputs]

    def deal(items: list[Device], start: int = 0) -> None:
        for index, device in enumerate(items):
            plan.seats[seat_names[(start + index) % len(seat_names)]].append(device.id)

    # A hub with a keyboard or mouse on it is a ready-made workstation: give
    # those to the extra seats first, since seat0 usually uses built-in ports.
    deal(stations, start=1 if len(seat_names) > 1 else 0)
    for kind in ("combo", "keyboard", "mouse", "pointer"):
        deal([d for d in loose if d.kind == kind])
    deal([d for d in loose if d.kind == "sound" and d.id.startswith("usb:")])
    return plan
