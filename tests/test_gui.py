import json
import threading
import unittest
import urllib.error
import urllib.request

from openseat.backend import DemoBackend
from openseat.gui import ApiError, App, make_server
from openseat.logind import LogindError
from openseat.plan import parse_plan


class AppTests(unittest.TestCase):
    def setUp(self):
        self.app = App(DemoBackend())

    def seats(self):
        return {d["id"]: d["seat"] for d in self.app.state()["devices"]}

    def test_state_starts_with_everything_on_seat0(self):
        state = self.app.state()
        self.assertTrue(state["demo"])
        self.assertEqual(state["seats"], ["seat0"])
        self.assertEqual(set(self.seats().values()), {"seat0"})

    def test_apply_moves_only_changed_devices(self):
        wanted = dict(self.seats(), **{"pci:0000:01:00.0": "seat1", "usb:1-1": "seat1"})
        out = self.app.apply({"assignments": wanted})
        self.assertEqual([(r["id"], r["to"], r["ok"]) for r in out["results"]],
                         [("pci:0000:01:00.0", "seat1", True), ("usb:1-1", "seat1", True)])
        self.assertEqual(out["state"]["seats"], ["seat0", "seat1"])
        self.assertEqual(self.app.apply({"assignments": wanted})["results"], [])

    def test_hub_moves_first_so_explicit_member_choice_wins(self):
        out = self.app.apply({"assignments": {"usb:1-3.2": "seat2", "hub:1-3": "seat1"}})
        self.assertEqual([r["id"] for r in out["results"]], ["hub:1-3", "usb:1-3.2"])
        seats = self.seats()
        self.assertEqual((seats["hub:1-3"], seats["usb:1-3.1"], seats["usb:1-3.2"]), ("seat1", "seat1", "seat2"))

    def test_apply_reports_a_failed_device_and_continues(self):
        real_attach = self.app.backend.attach

        def attach(seat, device):
            if device.id == "usb:1-1":
                raise LogindError("access denied")
            return real_attach(seat, device)

        self.app.backend.attach = attach
        out = self.app.apply({"assignments": {"usb:1-1": "seat1", "usb:1-2": "seat1"}})
        self.assertEqual([(r["id"], r["ok"]) for r in out["results"]], [("usb:1-1", False), ("usb:1-2", True)])
        self.assertEqual(out["results"][0]["error"], "access denied")
        self.assertEqual(self.seats()["usb:1-2"], "seat1")

    def test_apply_rejects_bad_input(self):
        for body in ({}, {"assignments": []}, {"assignments": {"usb:9-9": "seat1"}},
                     {"assignments": {"usb:1-1": "desk"}}, {"assignments": {"usb:1-1": 5}}):
            with self.subTest(body=body), self.assertRaises(ApiError):
                self.app.apply(body)
        self.assertEqual(set(self.seats().values()), {"seat0"})

    def test_auto_suggests_without_changing_anything(self):
        out = self.app.auto({})
        self.assertEqual(out["assignments"]["pci:0000:01:00.0"], "seat1")
        self.assertEqual(out["assignments"]["usb:1-3.1"], "seat1")  # follows hub:1-3
        self.assertEqual(out["assignments"]["usb:1-1"], "seat0")
        self.assertEqual(set(self.seats().values()), {"seat0"})

    def test_export_then_import_round_trips(self):
        wanted = self.app.auto({})["assignments"]
        toml = self.app.export_plan({"assignments": wanted})["toml"]
        self.assertNotIn("usb:1-3.1", parse_plan(toml).assignments())  # covered by its hub
        back = self.app.import_plan({"toml": toml})
        self.assertEqual(back["assignments"], wanted)
        self.assertEqual(back["missing"], [])

    def test_import_reports_missing_devices_and_bad_toml(self):
        out = self.app.import_plan({"toml": '[seats.seat1]\ndevices = ["usb:9-9"]'})
        self.assertEqual(out["missing"], ["usb:9-9"])
        with self.assertRaises(ApiError):
            self.app.import_plan({"toml": "nope ["})

    def test_reset_returns_everything_to_seat0(self):
        self.app.apply({"assignments": {"hub:1-3": "seat1"}})
        self.assertEqual(self.app.reset({})["state"]["seats"], ["seat0"])

    def test_identify_names_an_input_device(self):
        self.assertEqual(self.app.identify({"timeout": 0.1})["id"], "usb:1-1")
        with self.assertRaises(ApiError):
            self.app.identify({"timeout": "soon"})


class HttpTests(unittest.TestCase):
    def setUp(self):
        self.server, url = make_server(demo=True, token="secret-token")
        self.base = url.split("/#")[0]
        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)

    def call(self, path, body=None, headers=None, method=None):
        data = None if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode())
        req = urllib.request.Request(self.base + path, data=data, headers=headers or {}, method=method)
        try:
            with urllib.request.urlopen(req, timeout=5) as res:
                return res.status, res.read(), res.headers
        except urllib.error.HTTPError as err:
            return err.code, err.read(), err.headers

    def test_page_is_served_and_listens_on_loopback_only(self):
        status, body, headers = self.call("/")
        self.assertEqual(status, 200)
        self.assertIn(b"<title>openseat</title>", body)
        self.assertIn("frame-ancestors 'none'", headers["Content-Security-Policy"])
        self.assertEqual(self.server.server_address[0], "127.0.0.1")

    def test_api_needs_the_token(self):
        self.assertEqual(self.call("/api/state", {})[0], 403)
        self.assertEqual(self.call("/api/state", {}, {"X-Openseat-Token": "wrong"})[0], 403)
        status, body, _ = self.call("/api/state", {}, {"X-Openseat-Token": "secret-token"})
        self.assertEqual(status, 200)
        self.assertEqual(len(json.loads(body)["devices"]), 9)

    def test_foreign_host_header_is_refused(self):
        headers = {"X-Openseat-Token": "secret-token", "Host": "evil.example"}
        self.assertEqual(self.call("/api/state", {}, headers)[0], 403)
        self.assertEqual(self.call("/", headers={"Host": "evil.example"})[0], 403)

    def test_errors_are_json_with_the_right_status(self):
        auth = {"X-Openseat-Token": "secret-token"}
        self.assertEqual(self.call("/api/nope", {}, auth)[0], 404)
        self.assertEqual(self.call("/api/apply", b"{not json", auth)[0], 400)
        self.assertEqual(self.call("/api/apply", b"[1]", auth)[0], 400)
        status, body, _ = self.call("/api/apply", {"assignments": {"usb:1-1": "bad seat"}}, auth)
        self.assertEqual(status, 400)
        self.assertIn("invalid seat name", json.loads(body)["error"])
        self.assertEqual(self.call("/api/apply", b"x" * (300 * 1024), auth)[0], 413)


if __name__ == "__main__":
    unittest.main()
