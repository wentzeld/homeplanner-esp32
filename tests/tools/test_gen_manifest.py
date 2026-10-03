#!/usr/bin/env python3
"""Tests for tools/gen_manifest.py (the browser installer's manifest). Run: python3 tests/tools/test_gen_manifest.py"""
import copy
import json
import os
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True  # no __pycache__ in tools/
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import gen_manifest  # noqa: E402

# Shaped like the flasher_args.json idf.py writes for this project.
FLASHER_ARGS = {
    "write_flash_args": ["--flash_mode", "dio", "--flash_size", "keep", "--flash_freq", "80m"],
    "flash_settings": {"flash_mode": "dio", "flash_size": "keep", "flash_freq": "80m"},
    "flash_files": {
        "0x2000": "bootloader/bootloader.bin",
        "0x20000": "homeplanner.bin",
        "0x8000": "partition_table/partition-table.bin",
        "0x19000": "ota_data_initial.bin",
    },
    "bootloader": {"offset": "0x2000", "file": "bootloader/bootloader.bin", "encrypted": "false"},
    "app": {"offset": "0x20000", "file": "homeplanner.bin", "encrypted": "false"},
    "partition-table": {"offset": "0x8000", "file": "partition_table/partition-table.bin", "encrypted": "false"},
    "otadata": {"offset": "0x19000", "file": "ota_data_initial.bin", "encrypted": "false"},
    "extra_esptool_args": {"after": "hard_reset", "before": "default_reset", "stub": True, "chip": "esp32p4"},
}
CONTENTS = {
    "bootloader/bootloader.bin": b"boot",
    "partition_table/partition-table.bin": b"table",
    "ota_data_initial.bin": b"otadata",
    "homeplanner.bin": b"app",
}


class GenManifestTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.build = os.path.join(self.tmp.name, "build")
        self.out = os.path.join(self.tmp.name, "release")
        self.write_build(FLASHER_ARGS, CONTENTS)

    def tearDown(self):
        self.tmp.cleanup()

    def write_build(self, args, contents):
        for rel, data in contents.items():
            path = os.path.join(self.build, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        with open(os.path.join(self.build, "flasher_args.json"), "w") as f:
            json.dump(args, f)

    def assert_fails(self, args=None, version="1.2.4", match=""):
        if args is not None:
            self.write_build(args, {})
        with self.assertRaises(gen_manifest.ManifestError) as cm:
            gen_manifest.collect(self.build, self.out, version)
        self.assertIn(match, str(cm.exception))
        self.assertFalse(os.path.exists(os.path.join(self.out, "manifest.json")))

    def test_manifest_and_files(self):
        manifest = gen_manifest.collect(self.build, self.out, "1.2.4")
        with open(os.path.join(self.out, "manifest.json")) as f:
            self.assertEqual(json.load(f), manifest)
        self.assertEqual(manifest["name"], "HomePlanner")
        self.assertEqual(manifest["version"], "1.2.4")
        self.assertIs(manifest["new_install_prompt_erase"], True)
        self.assertEqual(manifest["new_install_improv_wait_time"], 0)
        self.assertEqual(len(manifest["builds"]), 1)
        self.assertEqual(manifest["builds"][0]["chipFamily"], "ESP32-P4")
        self.assertEqual(manifest["builds"][0]["parts"], [
            {"path": "bootloader.bin", "offset": 0x2000},
            {"path": "partition-table.bin", "offset": 0x8000},
            {"path": "ota_data_initial.bin", "offset": 0x19000},
            {"path": "homeplanner.bin", "offset": 0x20000},
        ])
        expected = {"bootloader.bin": b"boot", "partition-table.bin": b"table",
                    "ota_data_initial.bin": b"otadata", "homeplanner.bin": b"app"}
        self.assertEqual(sorted(os.listdir(self.out)), sorted(list(expected) + ["manifest.json"]))
        for name, data in expected.items():
            with open(os.path.join(self.out, name), "rb") as f:
                self.assertEqual(f.read(), data, name)

    def test_missing_part_entry(self):
        for key in gen_manifest.EXPECTED:
            args = copy.deepcopy(FLASHER_ARGS)
            del args[key]
            with self.subTest(key=key):
                self.assert_fails(args, match=f"no {key} part")

    def test_missing_file(self):
        os.remove(os.path.join(self.build, "ota_data_initial.bin"))
        self.assert_fails(match="missing or empty file")

    def test_empty_file(self):
        open(os.path.join(self.build, "homeplanner.bin"), "wb").close()
        self.assert_fails(match="missing or empty file")

    def test_moved_offset(self):
        args = copy.deepcopy(FLASHER_ARGS)
        args["app"]["offset"] = "0x10000"
        args["flash_files"] = {("0x10000" if k == "0x20000" else k): v for k, v in args["flash_files"].items()}
        self.assert_fails(args, match="expected 0x20000")

    def test_part_not_in_flash_files(self):
        args = copy.deepcopy(FLASHER_ARGS)
        del args["flash_files"]["0x19000"]
        self.assert_fails(args, match="flash_files has no ota_data_initial.bin")

    def test_unknown_extra_part(self):
        args = copy.deepcopy(FLASHER_ARGS)
        args["flash_files"]["0x820000"] = "storage.bin"
        self.assert_fails(args, match="0x820000 storage.bin")

    def test_encrypted_part(self):
        args = copy.deepcopy(FLASHER_ARGS)
        args["app"]["encrypted"] = "true"
        self.assert_fails(args, match="encrypted")

    def test_wrong_chip(self):
        args = copy.deepcopy(FLASHER_ARGS)
        args["extra_esptool_args"]["chip"] = "esp32s3"
        self.assert_fails(args, match="esp32p4")

    def test_bad_version(self):
        for version in ("", "v1.2.4", "1.2.4-beta", None):
            with self.subTest(version=version):
                self.assert_fails(version=version, match="version")

    def test_missing_flasher_args(self):
        os.remove(os.path.join(self.build, "flasher_args.json"))
        self.assert_fails(match="can't read")

    def test_command_line(self):
        self.assertEqual(gen_manifest.main(["gen_manifest.py", self.build, self.out, "1.2.4"]), 0)
        self.assertTrue(os.path.isfile(os.path.join(self.out, "manifest.json")))
        self.assertEqual(gen_manifest.main(["gen_manifest.py", self.build]), 2)
        os.remove(os.path.join(self.build, "homeplanner.bin"))
        self.assertEqual(gen_manifest.main(["gen_manifest.py", self.build, self.out + "2", "1.2.4"]), 1)


if __name__ == "__main__":
    unittest.main()
