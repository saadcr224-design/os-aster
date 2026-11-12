import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from openseat import cli
from openseat.devices import discover, is_boot_gpu, parse_bitmap
from openseat.logind import LogindError, Runner, validate_seat
from openseat.plan import Plan, PlanError, auto_plan, diff, parse_plan

from .fakesys import two_seat_pc


class FakeSysCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.fs = two_seat_pc(Path(tmp.name))
        self.root = Path(tmp.name)

    def devices(self):
        return discover(self.fs.roots)

    def by_id(self):
        return {d.id: d for d in self.devices()}


class DiscoveryTests(FakeSysCase):
    def test_finds_and_classifies_every_device(self):
        kinds = {d.id: d.kind for d in self.devices()}
        self.assertEqual(
            kinds,
            {
                "pci:0000:00:02.0": "gpu",
                "pci:0000:01:00.0": "gpu",
                "usb:1-1": "keyboard",
                "usb:1-2": "mouse",
                "usb:1-3.1": "keyboard",
                "usb:1-3.2": "mouse",
                "pci:0000:00:1f.3": "sound",
                "usb:1-4": "sound",
                "hub:1-3": "usb-hub",
                "sys:LNXPWRBN:00": "other",
            },
        )

    def test_keyboard_interfaces_are_grouped_into_one_device(self):
        keyboard = self.by_id()["usb:1-1"]
        self.assertEqual(keyboard.name, "Acme Keyboard")
        self.assertEqual(len(keyboard.nodes), 2)

    def test_hub_membership_and_root_hub_is_ignored(self):
        devices = self.by_id()
        self.assertEqual(devices["usb:1-3.1"].hub, "hub:1-3")
        self.assertIsNone(devices["usb:1-1"].hub)
        self.assertNotIn("hub:usb1", devices)

    def test_connectors_and_boot_gpu(self):
        devices = self.by_id()
        self.assertEqual(devices["pci:0000:00:02.0"].details, ["DP-1: disconnected", "HDMI-A-1: connected"])
        self.assertTrue(is_boot_gpu(devices["pci:0000:00:02.0"]))
        self.assertFalse(is_boot_gpu(devices["pci:0000:01:00.0"]))

    def test_seat_is_read_from_udev_database(self):
        self.fs.set_seat("c226:1", "seat1")  # drm card1 has a device node
        self.fs.set_seat("+input:input3", "seat1")  # input nodes do not
        devices = self.by_id()
        self.assertEqual(devices["pci:0000:01:00.0"].seat, "seat1")
        self.assertEqual(devices["usb:1-3.1"].seat, "seat1")
        self.assertEqual(devices["usb:1-1"].seat, "seat0")

    def test_partly_moved_device_reports_mixed(self):
        self.fs.set_seat("+input:input0", "seat1")
        self.assertEqual(self.by_id()["usb:1-1"].seat, "mixed")

    def test_missing_sysfs_gives_no_devices(self):
        self.assertEqual(discover(type(self.fs.roots)(sys=self.root / "nope", udev_data=self.root / "nope")), [])

    def test_parse_bitmap_is_high_word_first(self):
        self.assertEqual(parse_bitmap("1 0"), 1 << 64)
        self.assertTrue(parse_bitmap("70000 0 0 0 0") >> 0x110 & 1)


class PlanTests(FakeSysCase):
    def test_auto_plan_gives_hub_station_to_second_seat(self):
        plan = auto_plan(self.devices())
        self.assertEqual(
            plan.seats,
            {
                "seat0": ["pci:0000:00:02.0", "usb:1-1", "usb:1-2", "usb:1-4"],
                "seat1": ["pci:0000:01:00.0", "hub:1-3"],
            },
        )

    def test_auto_plan_round_trips_through_toml(self):
        devices = self.devices()
        plan = auto_plan(devices)
        self.assertEqual(parse_plan(plan.to_toml(devices)).seats, plan.seats)

    def test_auto_plan_with_one_gpu_has_one_seat(self):
        devices = [d for d in self.devices() if d.id != "pci:0000:01:00.0"]
        self.assertEqual(list(auto_plan(devices).seats), ["seat0"])

    def test_diff_moves_only_what_differs(self):
        self.fs.set_seat("c226:1", "seat1")
        plan = Plan({"seat1": ["pci:0000:01:00.0", "hub:1-3", "usb:9-9"]})
        result = diff(plan, self.devices())
        self.assertEqual([(a.device.id, a.seat) for a in result.actions], [("hub:1-3", "seat1")])
        self.assertEqual([d.id for d in result.unchanged], ["pci:0000:01:00.0"])
        self.assertEqual(result.missing, ["usb:9-9"])

    def test_reset_others_returns_strays_but_not_hub_children(self):
        for udev_id in ("+input:input0", "+input:input1", "+input:input3"):
            self.fs.set_seat(udev_id, "seat1")  # usb:1-1 (loose) and usb:1-3.1 (on the hub)
        plan = Plan({"seat1": ["hub:1-3"]})
        self.assertEqual([a.device.id for a in diff(plan, self.devices()).actions], ["hub:1-3"])
        moved = {a.device.id: a.seat for a in diff(plan, self.devices(), reset_others=True).actions}
        self.assertEqual(moved, {"hub:1-3": "seat1", "usb:1-1": "seat0"})

    def test_explicit_device_overrides_its_hub(self):
        plan = Plan({"seat1": ["hub:1-3"], "seat2": ["usb:1-3.2"]})
        actions = [(a.device.id, a.seat) for a in diff(plan, self.devices()).actions]
        self.assertEqual(actions, [("hub:1-3", "seat1"), ("usb:1-3.2", "seat2")])

    def test_plan_validation(self):
        for text in (
            "",
            "[seats.desk1]\ndevices = []",
            "[seats.seat1]\ndevices = 'usb:1-1'",
            '[seats.seat1]\ndevices = ["usb:1-1"]\n[seats.seat2]\ndevices = ["usb:1-1"]',
            "not toml [",
        ):
            with self.subTest(text=text), self.assertRaises(PlanError):
                parse_plan(text)


class LogindTests(unittest.TestCase):
    def test_seat_names(self):
        self.assertEqual(validate_seat("seat-left_1"), "seat-left_1")
        for bad in ("seat", "desk1", "seat 1", "seat1;reboot", ""):
            with self.subTest(bad=bad), self.assertRaises(LogindError):
                validate_seat(bad)

    def test_failed_command_raises_with_stderr(self):
        with mock.patch("shutil.which", return_value="/bin/sh"), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(LogindError, "boom"):
                Runner().run(["sh", "-c", "echo boom >&2; exit 3"])

    def test_missing_loginctl_is_reported(self):
        with mock.patch("shutil.which", return_value=None):
            with self.assertRaisesRegex(LogindError, "not found"):
                Runner().attach("seat1", ["/sys/x"])


class CliTests(FakeSysCase):
    def run_cli(self, *argv):
        env = {"OPENSEAT_SYSFS": str(self.fs.sys), "OPENSEAT_UDEV_DATA": str(self.fs.udev)}
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.dict(os.environ, env), contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def test_list_hides_minor_devices_unless_all(self):
        _, out, _ = self.run_cli("list")
        self.assertIn("usb:1-3.1", out)
        self.assertIn("(on hub:1-3)", out)
        self.assertIn("HDMI-A-1: connected", out)
        self.assertNotIn("Power Button", out)
        self.assertIn("Power Button", self.run_cli("list", "--all")[1])

    def test_list_json(self):
        code, out, _ = self.run_cli("list", "--json")
        self.assertEqual(code, 0)
        gpu = next(d for d in json.loads(out) if d["id"] == "pci:0000:01:00.0")
        self.assertTrue(gpu["syspaths"][0].endswith("0000:01:00.0/drm/card1"))

    def test_attach_dry_run_prints_loginctl_command_for_every_node(self):
        code, out, _ = self.run_cli("attach", "-n", "seat1", "usb:1-1")
        self.assertEqual(code, 0)
        line = next(l for l in out.splitlines() if l.startswith("would run:"))
        self.assertIn("loginctl attach seat1 ", line)
        self.assertIn("/input/input0", line)
        self.assertIn("/input/input1", line)

    def test_attach_rejects_unknown_device_and_bad_seat(self):
        code, _, err = self.run_cli("attach", "-n", "seat1", "usb:7-7")
        self.assertEqual(code, 2)
        self.assertIn("unknown device id: usb:7-7", err)
        self.assertEqual(self.run_cli("attach", "-n", "desk", "usb:1-1")[0], 2)

    def test_detach_targets_seat0(self):
        _, out, _ = self.run_cli("detach", "-n", "usb:1-2")
        self.assertIn("loginctl attach seat0 ", out)

    def test_auto_then_apply_dry_run(self):
        _, toml, _ = self.run_cli("auto")
        plan = self.root / "seats.toml"
        plan.write_text(toml)
        code, out, _ = self.run_cli("apply", "-n", str(plan))
        self.assertEqual(code, 0)
        self.assertIn("pci:0000:01:00.0 (PCI 10de:0001): seat0 -> seat1", out)
        self.assertIn("2 to change, 4 already correct, 0 missing", out)

    def test_apply_strict_fails_on_missing_device(self):
        plan = self.root / "seats.toml"
        plan.write_text('[seats.seat1]\ndevices = ["usb:9-9"]\n')
        code, _, err = self.run_cli("apply", "-n", "--strict", str(plan))
        self.assertEqual(code, 1)
        self.assertIn("usb:9-9 is in the plan but not connected", err)
        self.assertEqual(self.run_cli("apply", "-n", str(plan))[0], 0)

    def test_apply_reports_unreadable_plan(self):
        code, _, err = self.run_cli("apply", str(self.root / "missing.toml"))
        self.assertEqual(code, 2)
        self.assertIn("cannot read", err)

    def test_seats_warns_about_seat_without_graphics(self):
        self.fs.set_seat("+input:input2", "seat1")
        _, out, _ = self.run_cli("seats")
        self.assertIn("seat1  (no graphics card", out)

    def test_reset_dry_run(self):
        self.assertIn("loginctl flush-devices", self.run_cli("reset", "-n")[1])

    def test_identify_reports_the_device_that_sent_events(self):
        # eventN nodes are FIFOs here; only the second keyboard "types".
        import struct

        for i in range(7):
            os.mkfifo(self.fs.dev_input / f"event{i}")
        writer = os.open(self.fs.dev_input / "event3", os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, writer)
        os.write(writer, struct.pack("llHHi", 0, 0, 1, 30, 1))  # EV_KEY KEY_A down
        code, out, _ = self.run_cli("identify", "--timeout", "0.3", "--dev-input", str(self.fs.dev_input))
        self.assertEqual(code, 0)
        self.assertIn("usb:1-3.1", out)
        self.assertNotIn("usb:1-1 ", out)


if __name__ == "__main__":
    unittest.main()
