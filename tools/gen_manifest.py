#!/usr/bin/env python3
"""Collect the files for the browser installer (docs/install.html, ESP Web Tools) into one folder.

    tools/gen_manifest.py BUILD_DIR OUT_DIR VERSION
    e.g. tools/gen_manifest.py build build/release 1.2.4

Reads BUILD_DIR/flasher_args.json (written by idf.py build), copies the four flash parts into OUT_DIR as
bootloader.bin, partition-table.bin, ota_data_initial.bin and homeplanner.bin, and writes OUT_DIR/manifest.json
with paths relative to the manifest. Fails if a part, its file or its expected offset is missing or different,
so a changed partition layout can't silently produce a broken installer.
"""
import json
import os
import re
import shutil
import sys

# flasher_args.json key -> (offset, file name in the release). homeplanner.bin keeps its name: panels
# update over the air from the release asset with exactly that name.
EXPECTED = {
    "bootloader": (0x2000, "bootloader.bin"),
    "partition-table": (0x8000, "partition-table.bin"),
    "otadata": (0x19000, "ota_data_initial.bin"),
    "app": (0x20000, "homeplanner.bin"),
}
CHIP_FAMILY = "ESP32-P4"


class ManifestError(Exception):
    pass


def collect(build_dir, out_dir, version):
    """Copy the parts and write manifest.json. Returns the manifest (a dict)."""
    if not re.fullmatch(r"[0-9]+(\.[0-9]+)*", version or ""):
        raise ManifestError(f"version must look like 1.2.0, not {version!r}")
    args_path = os.path.join(build_dir, "flasher_args.json")
    try:
        with open(args_path, encoding="utf-8") as f:
            args = json.load(f)
    except (OSError, ValueError) as e:
        raise ManifestError(f"can't read {args_path}: {e}") from e

    chip = args.get("extra_esptool_args", {}).get("chip")
    if chip != "esp32p4":
        raise ManifestError(f"{args_path} is for chip {chip!r}, expected esp32p4")

    flash_files = {int(k, 16): v for k, v in args.get("flash_files", {}).items()}
    sources = []
    for key, (offset, name) in EXPECTED.items():
        part = args.get(key)
        if not isinstance(part, dict) or "offset" not in part or "file" not in part:
            raise ManifestError(f"{args_path} has no {key} part")
        if int(part["offset"], 16) != offset:
            raise ManifestError(f"{key} is at {part['offset']}, expected {offset:#x} "
                                "(partition layout changed? update EXPECTED in tools/gen_manifest.py)")
        if str(part.get("encrypted", "false")).lower() != "false":
            raise ManifestError(f"{key} is encrypted; the installer only writes plain images")
        if flash_files.get(offset) != part["file"]:
            raise ManifestError(f"flash_files has no {part['file']} at {offset:#x}")
        src = os.path.join(build_dir, part["file"])
        if not os.path.isfile(src) or os.path.getsize(src) == 0:
            raise ManifestError(f"missing or empty file {src}")
        sources.append((offset, name, src))
    extra = sorted(set(flash_files) - {o for o, _ in EXPECTED.values()})
    if extra:
        raise ManifestError("flash_files has parts the installer doesn't know: " +
                            ", ".join(f"{o:#x} {flash_files[o]}" for o in extra))

    os.makedirs(out_dir, exist_ok=True)
    parts = []
    for offset, name, src in sorted(sources):
        shutil.copyfile(src, os.path.join(out_dir, name))
        parts.append({"path": name, "offset": offset})
    manifest = {
        "name": "HomePlanner",
        "version": version,
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [{"chipFamily": CHIP_FAMILY, "parts": parts}],
    }
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    return manifest


def main(argv):
    if len(argv) != 4:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    try:
        manifest = collect(argv[1], argv[2], argv[3])
    except ManifestError as e:
        print(f"gen_manifest: {e}", file=sys.stderr)
        return 1
    for p in manifest["builds"][0]["parts"]:
        print(f"{p['offset']:#08x}  {p['path']}")
    print(f"wrote {os.path.join(argv[2], 'manifest.json')} (version {manifest['version']})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
