"""Command line interface for openseat."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import __version__
from .devices import DEFAULT_SEAT, Device, discover
from .identify import IdentifyError, Watcher
from .logind import LogindError, Runner, capture, validate_seat
from .plan import PlanError, auto_plan, diff, load_plan


def _find(devices: list[Device], ids: list[str]) -> list[Device]:
    by_id = {d.id: d for d in devices}
    unknown = [i for i in ids if i not in by_id]
    if unknown:
        raise PlanError(f"unknown device id: {', '.join(unknown)} (see `openseat list --all`)")
    return [by_id[i] for i in ids]


def _visible(devices: list[Device], show_all: bool) -> list[Device]:
    return devices if show_all else [d for d in devices if d.kind != "other"]


def cmd_list(args) -> int:
    devices = _visible(discover(), args.all)
    if args.json:
        print(
            json.dumps(
                [
                    {
                        "id": d.id,
                        "kind": d.kind,
                        "name": d.name,
                        "seat": d.seat,
                        "hub": d.hub,
                        "details": d.details,
                        "syspaths": d.syspaths,
                    }
                    for d in devices
                ],
                indent=2,
            )
        )
        return 0
    if not devices:
        print("No seat-assignable devices found.")
        return 0
    rows = [("SEAT", "KIND", "ID", "NAME")]
    for d in devices:
        name = d.name + (f"  (on {d.hub})" if d.hub else "")
        rows.append((d.seat, d.kind, d.id, name))
        rows += [("", "", "", f"  {detail}") for detail in d.details]
    widths = [max(len(r[i]) for r in rows) for i in range(3)]
    for r in rows:
        print("  ".join(c.ljust(w) for c, w in zip(r, widths)) + "  " + r[3])
    return 0


def cmd_seats(args) -> int:
    devices = _visible(discover(), False)
    seats: dict[str, list[Device]] = {DEFAULT_SEAT: []}
    for d in devices:
        seats.setdefault(d.seat, []).append(d)
    for seat in sorted(seats):
        members = seats[seat]
        has_gpu = any(d.kind == "gpu" for d in members)
        note = "" if has_gpu or seat == "mixed" else "  (no graphics card: logind will not start a login screen)"
        print(f"{seat}{note}")
        for d in members:
            print(f"  {d.kind:<9} {d.id:<22} {d.name}")
    return 0


def cmd_attach(args) -> int:
    validate_seat(args.seat)
    runner = Runner(dry_run=args.dry_run)
    for device in _find(discover(), args.devices):
        runner.attach(args.seat, device.syspaths)
        print(f"{device.id} ({device.name}) -> {args.seat}")
    return 0


def cmd_detach(args) -> int:
    args.seat = DEFAULT_SEAT
    return cmd_attach(args)


def cmd_apply(args) -> int:
    plan = load_plan(args.plan)
    result = diff(plan, discover(), reset_others=args.reset_others)
    runner = Runner(dry_run=args.dry_run)
    for action in result.actions:
        runner.attach(action.seat, action.device.syspaths)
        print(f"{action.device.id} ({action.device.name}): {action.device.seat} -> {action.seat}")
    for dev_id in result.missing:
        print(f"warning: {dev_id} is in the plan but not connected", file=sys.stderr)
    verb = "to change" if args.dry_run else "changed"
    print(f"{len(result.actions)} {verb}, {len(result.unchanged)} already correct, {len(result.missing)} missing")
    return 1 if result.missing and args.strict else 0


def cmd_auto(args) -> int:
    devices = discover()
    plan = auto_plan(devices)
    if len(plan.seats) < 2:
        gpus = sum(d.kind == "gpu" for d in devices)
        print(
            f"# Found {gpus} graphics card(s). Linux multiseat needs one graphics card per seat,\n"
            "# so there is nothing to split yet. Add a second card (a cheap USB or PCI one works).",
            file=sys.stderr,
        )
    print(plan.to_toml(devices), end="")
    return 0


def cmd_reset(args) -> int:
    Runner(dry_run=args.dry_run).flush()
    print("All devices returned to seat0.")
    return 0


def cmd_identify(args) -> int:
    """Report which keyboard or mouse the user is touching."""
    try:
        watcher = Watcher(discover(), args.dev_input)
    except IdentifyError as exc:
        print(f"Cannot watch input devices: {exc}", file=sys.stderr)
        return 1
    print(f"Press a key or move a mouse (watching {len(watcher)} nodes, {args.timeout:g}s)... Ctrl+C to stop.")
    try:
        with watcher:
            for device in watcher.events(args.timeout):
                print(f"{device.id:<22} {device.kind:<9} {device.name}  [{device.seat}]")
    except KeyboardInterrupt:
        print()
    return 0


def cmd_gui(args) -> int:
    from .gui import serve

    return serve(port=args.port, open_browser=not args.no_browser, demo=args.demo)


def cmd_doctor(args) -> int:
    devices = discover()
    problems = 0

    def check(ok: bool, good: str, bad: str) -> None:
        nonlocal problems
        problems += not ok
        print(f"[{'ok' if ok else '!!'}] {good if ok else bad}")

    check(
        Path("/run/systemd/seats").is_dir(),
        "systemd-logind is running",
        "systemd-logind is not running; openseat depends on it",
    )
    gpus = [d for d in devices if d.kind == "gpu"]
    check(
        len(gpus) >= 2,
        f"{len(gpus)} graphics cards found",
        f"{len(gpus)} graphics card(s) found; each seat needs its own card",
    )
    keyboards = [d for d in devices if d.kind in ("keyboard", "combo")]
    check(
        len(keyboards) >= 2,
        f"{len(keyboards)} keyboards found",
        f"{len(keyboards)} keyboard(s) found; each seat needs its own",
    )
    seats = {d.seat for d in gpus}
    if len(gpus) >= 2:
        check(
            len(seats) >= 2,
            f"graphics cards are spread over {len(seats)} seats",
            "all graphics cards are on one seat; run `openseat auto` to get a plan",
        )
    dm = capture(["systemctl", "show", "-p", "Id", "--value", "display-manager.service"]).strip()
    if dm:
        known = dm.split(".")[0] in ("gdm", "gdm3", "lightdm")
        print(
            f"[{'ok' if known else '??'}] display manager: {dm}"
            + ("" if known else " (multiseat is known to work with GDM and LightDM; check this one's docs)")
        )
    else:
        print("[??] no display manager detected; a seat needs one to show a login screen")
    return 1 if problems else 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="openseat", description="Turn one Linux PC into several independent workstations."
    )
    parser.add_argument("--version", action="version", version=f"openseat {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)

    def add(name: str, func, help: str, dry_run: bool = False) -> argparse.ArgumentParser:
        p = sub.add_parser(name, help=help, description=help)
        p.set_defaults(func=func)
        if dry_run:
            p.add_argument("-n", "--dry-run", action="store_true", help="show what would change, change nothing")
        return p

    p = add("list", cmd_list, "List graphics cards, keyboards, mice, sound cards and USB hubs.")
    p.add_argument("--all", action="store_true", help="include power buttons and other minor devices")
    p.add_argument("--json", action="store_true", help="machine-readable output")

    add("seats", cmd_seats, "Show each seat and the devices assigned to it.")

    p = add("attach", cmd_attach, "Assign devices to a seat.", dry_run=True)
    p.add_argument("seat", help="seat name, e.g. seat1")
    p.add_argument("devices", nargs="+", metavar="device-id")

    p = add("detach", cmd_detach, "Return devices to seat0.", dry_run=True)
    p.add_argument("devices", nargs="+", metavar="device-id")

    p = add("apply", cmd_apply, "Apply a seat plan file.", dry_run=True)
    p.add_argument("plan", help="TOML plan file (see `openseat auto`)")
    p.add_argument("--reset-others", action="store_true", help="move devices not in the plan back to seat0")
    p.add_argument("--strict", action="store_true", help="fail if a planned device is not connected")

    add("auto", cmd_auto, "Print a suggested seat plan for the connected hardware.")
    add("reset", cmd_reset, "Remove every assignment; all devices return to seat0.", dry_run=True)

    p = add("identify", cmd_identify, "Show which keyboard or mouse you are touching.")
    p.add_argument("--timeout", type=float, default=30, help="seconds to watch (default 30)")
    p.add_argument("--dev-input", default="/dev/input", help=argparse.SUPPRESS)

    add("doctor", cmd_doctor, "Check that this machine is ready for multiseat.")

    p = add("gui", cmd_gui, "Open the graphical seat manager in your web browser.")
    p.add_argument("--port", type=int, default=0, help="port to listen on (default: any free port)")
    p.add_argument("--no-browser", action="store_true", help="print the address instead of opening a browser")
    p.add_argument("--demo", action="store_true", help="use simulated hardware; changes nothing on this PC")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (PlanError, LogindError) as exc:
        print(f"openseat: {exc}", file=sys.stderr)
        return 2
    except BrokenPipeError:
        return 0
