"""Graphical seat manager, served to the local web browser.

The server listens on 127.0.0.1 only.  Every API call must carry the random
token from the address printed at start-up, and must name the server's own
host, so other websites open in the same browser cannot drive it.
"""

from __future__ import annotations

import json
import secrets
import threading
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib import resources

from . import __version__
from .backend import DemoBackend, IdentifyError, RealBackend
from .devices import DEFAULT_SEAT, Device
from .logind import LogindError, validate_seat
from .plan import Plan, PlanError, auto_plan, parse_plan

MAX_BODY = 256 * 1024
MAX_IDENTIFY_SECONDS = 10.0


class ApiError(Exception):
    def __init__(self, message: str, status: HTTPStatus = HTTPStatus.BAD_REQUEST):
        super().__init__(message)
        self.status = status


def _device_json(device: Device) -> dict:
    return {
        "id": device.id,
        "kind": device.kind,
        "name": device.name,
        "seat": device.seat,
        "hub": device.hub,
        "details": device.details,
    }


def _assignments(body: dict, devices: list[Device]) -> dict[str, str]:
    raw = body.get("assignments")
    if not isinstance(raw, dict):
        raise ApiError("assignments must be an object of device id -> seat")
    known = {d.id for d in devices}
    out = {}
    for dev_id, seat in raw.items():
        if dev_id not in known:
            raise ApiError(f"unknown device: {dev_id}")
        if not isinstance(seat, str):
            raise ApiError(f"seat for {dev_id} must be a string")
        try:
            out[dev_id] = validate_seat(seat)
        except LogindError as exc:
            raise ApiError(str(exc)) from exc
    return out


class App:
    """The API, independent of HTTP so it is easy to test."""

    def __init__(self, backend) -> None:
        self.backend = backend
        self._lock = threading.Lock()

    def _visible(self) -> list[Device]:
        return [d for d in self.backend.devices() if d.kind != "other"]

    def state(self, _body: dict | None = None) -> dict:
        devices = self._visible()
        seats = sorted({DEFAULT_SEAT} | {d.seat for d in devices if d.seat != "mixed"})
        return {
            "version": __version__,
            "demo": self.backend.demo,
            "seats": seats,
            "devices": [_device_json(d) for d in devices],
        }

    def apply(self, body: dict) -> dict:
        with self._lock:
            devices = self._visible()
            wanted = _assignments(body, devices)
            # Hubs first: moving a hub carries its devices, explicit moves then win.
            todo = sorted(
                (d for d in devices if d.id in wanted and d.seat != wanted[d.id]),
                key=lambda d: d.kind != "usb-hub",
            )
            results = []
            for device in todo:
                seat = wanted[device.id]
                entry = {"id": device.id, "name": device.name, "from": device.seat, "to": seat}
                try:
                    entry["command"] = self.backend.attach(seat, device)
                    entry["ok"] = True
                except LogindError as exc:
                    entry["ok"] = False
                    entry["error"] = str(exc)
                results.append(entry)
            return {"results": results, "state": self.state()}

    def reset(self, _body: dict) -> dict:
        with self._lock:
            try:
                self.backend.reset()
            except LogindError as exc:
                raise ApiError(str(exc), HTTPStatus.INTERNAL_SERVER_ERROR) from exc
            return {"state": self.state()}

    def auto(self, _body: dict) -> dict:
        devices = self._visible()
        plan = auto_plan(devices)
        return {"assignments": self._expand(plan, devices), "seats": list(plan.seats)}

    @staticmethod
    def _expand(plan: Plan, devices: list[Device]) -> dict[str, str]:
        """Plan -> seat for every device (hub members follow their hub)."""
        chosen = plan.assignments()
        out = {}
        for device in devices:
            current = DEFAULT_SEAT if device.seat == "mixed" else device.seat
            out[device.id] = chosen.get(device.id) or chosen.get(device.hub or "") or current
        return out

    def export_plan(self, body: dict) -> dict:
        devices = self._visible()
        wanted = _assignments(body, devices)
        seats: dict[str, list[str]] = {}
        hub_seat = {d.id: wanted.get(d.id) for d in devices if d.kind == "usb-hub"}
        for device in devices:
            seat = wanted.get(device.id)
            if seat is None or (device.hub and hub_seat.get(device.hub) == seat):
                continue  # a device on a hub with the same seat is covered by the hub
            seats.setdefault(seat, []).append(device.id)
        return {"toml": Plan(dict(sorted(seats.items()))).to_toml(devices)}

    def import_plan(self, body: dict) -> dict:
        text = body.get("toml")
        if not isinstance(text, str):
            raise ApiError("toml must be a string")
        try:
            plan = parse_plan(text)
        except PlanError as exc:
            raise ApiError(str(exc)) from exc
        devices = self._visible()
        known = {d.id for d in devices}
        return {
            "assignments": self._expand(plan, devices),
            "seats": list(plan.seats),
            "missing": [i for i in plan.assignments() if i not in known],
        }

    def identify(self, body: dict) -> dict:
        try:
            timeout = float(body.get("timeout", 3))
        except (TypeError, ValueError) as exc:
            raise ApiError("timeout must be a number") from exc
        try:
            return {"id": self.backend.identify(max(0.1, min(timeout, MAX_IDENTIFY_SECONDS)))}
        except IdentifyError as exc:
            raise ApiError(str(exc), HTTPStatus.FORBIDDEN) from exc

    ROUTES = {
        "/api/state": state,
        "/api/apply": apply,
        "/api/reset": reset,
        "/api/auto": auto,
        "/api/export": export_plan,
        "/api/import": import_plan,
        "/api/identify": identify,
    }


def make_handler(app: App, token: str):
    page = resources.files("openseat").joinpath("gui.html").read_bytes()

    class Handler(BaseHTTPRequestHandler):
        server_version = f"openseat/{__version__}"

        def log_message(self, *_args) -> None:  # keep the terminal quiet
            pass

        def _send(self, status: HTTPStatus, body: bytes, content_type: str) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "default-src 'self' 'unsafe-inline'; frame-ancestors 'none'")
            self.end_headers()
            self.wfile.write(body)

        def _json(self, status: HTTPStatus, payload: dict) -> None:
            self._send(status, json.dumps(payload).encode(), "application/json")

        def _host_ok(self) -> bool:
            port = self.server.server_address[1]
            return self.headers.get("Host", "") in (f"127.0.0.1:{port}", f"localhost:{port}")

        def do_GET(self) -> None:
            if not self._host_ok():
                return self._json(HTTPStatus.FORBIDDEN, {"error": "bad host"})
            if self.path.split("?", 1)[0] == "/":
                return self._send(HTTPStatus.OK, page, "text/html; charset=utf-8")
            self._json(HTTPStatus.NOT_FOUND, {"error": "not found"})

        def do_POST(self) -> None:
            if not self._host_ok():
                return self._json(HTTPStatus.FORBIDDEN, {"error": "bad host"})
            if not secrets.compare_digest(self.headers.get("X-Openseat-Token", ""), token):
                return self._json(HTTPStatus.FORBIDDEN, {"error": "missing or wrong token; reopen the address openseat printed"})
            route = App.ROUTES.get(self.path)
            if route is None:
                return self._json(HTTPStatus.NOT_FOUND, {"error": "not found"})
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 <= length <= MAX_BODY:
                    raise ApiError("request too large", HTTPStatus.REQUEST_ENTITY_TOO_LARGE)
                body = json.loads(self.rfile.read(length) or b"{}")
                if not isinstance(body, dict):
                    raise ApiError("request body must be a JSON object")
                self._json(HTTPStatus.OK, route(app, body))
            except ValueError:
                self._json(HTTPStatus.BAD_REQUEST, {"error": "invalid JSON"})
            except ApiError as exc:
                self._json(exc.status, {"error": str(exc)})

    return Handler


def make_server(port: int = 0, demo: bool = False, token: str | None = None):
    """Create (but do not start) the server; returns (server, url)."""
    token = token or secrets.token_urlsafe(24)
    app = App(DemoBackend() if demo else RealBackend())
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(app, token))
    server.daemon_threads = True
    return server, f"http://127.0.0.1:{server.server_address[1]}/#{token}"


def serve(port: int = 0, open_browser: bool = True, demo: bool = False) -> int:
    try:
        server, url = make_server(port, demo)
    except OSError as exc:
        print(f"openseat: cannot start the GUI on port {port}: {exc.strerror}")
        return 2
    print(f"openseat GUI{' (demo: simulated hardware)' if demo else ''} is running at:\n\n  {url}\n")
    print("Keep this window open. Press Ctrl+C to stop.")
    if open_browser:
        threading.Thread(target=webbrowser.open, args=(url,), daemon=True).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    finally:
        server.server_close()
    return 0
