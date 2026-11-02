#!/usr/bin/env python3
"""Assemble the CrossX web flasher (GitHub Pages) from site/ plus built firmware images.

    python3 scripts/build_flasher_site.py --out build/site \
        --firmware crossx_x4=.pio/build/crossx_x4/firmware.bin \
        --firmware default=.pio/build/default/firmware.bin

Copies site/ into --out, places each image under firmware/, and writes manifest.json with the
size and SHA-256 the page verifies before it flashes anything. Envs without --firmware are
left out of the manifest; the page still offers a local .bin install.
"""

from __future__ import annotations

import argparse
import configparser
import datetime as dt
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The catalog the page shows, in order. Keys are PlatformIO env names.
BUILDS = {
    "crossx_x4": {
        "id": "crossx",
        "name": "CrossX",
        "channel": "Pre-alpha",
        "description": "A leaner CrossPoint Reader: English UI, no Bluetooth page-turner, and the CrossX sync foundation.",
        "file": "crossx-x4.bin",
        "versionSuffix": "-crossx",
        "requiresAck": True,
    },
    "default": {
        "id": "crosspoint",
        "name": "CrossPoint",
        "channel": "Plain reader",
        "description": "Full CrossPoint from the same source: every language and the page-turner. Install it to go back.",
        "file": "crosspoint-x4.bin",
        "versionSuffix": "",
        "requiresAck": False,
    },
}

ESP_IMAGE_MAGIC = 0xE9
ESP32_C3_CHIP_ID = 0x0005


def crosspoint_version() -> str:
    config = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=(";",))
    config.read(ROOT / "platformio.ini")
    return config["crosspoint"]["version"].strip()


def git_commit() -> str:
    sha = os.environ.get("GITHUB_SHA")
    if sha:
        return sha
    return subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, check=True, capture_output=True, text=True).stdout.strip()


def check_image(env: str, data: bytes) -> None:
    if len(data) < 24 or data[0] != ESP_IMAGE_MAGIC:
        sys.exit(f"{env}: not an ESP app image (magic byte 0xE9 missing)")
    chip = data[12] | (data[13] << 8)
    if chip != ESP32_C3_CHIP_ID:
        sys.exit(f"{env}: image chip id 0x{chip:04x} is not the ESP32-C3 (0x0005)")


def parse_firmware_args(values: list[str]) -> dict[str, Path]:
    images: dict[str, Path] = {}
    for value in values:
        env, sep, path = value.partition("=")
        if not sep or env not in BUILDS:
            sys.exit(f"--firmware expects ENV=PATH with ENV in {sorted(BUILDS)}; got {value!r}")
        images[env] = Path(path)
    return images


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--firmware", action="append", default=[], metavar="ENV=PATH")
    args = parser.parse_args()

    images = parse_firmware_args(args.firmware)
    out: Path = args.out
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(ROOT / "site", out, ignore=shutil.ignore_patterns("test", "hw", "package.json"))

    version = crosspoint_version()
    builds = []
    for env, meta in BUILDS.items():
        if env not in images:
            continue
        data = images[env].read_bytes()
        check_image(env, data)
        target = out / "firmware" / meta["file"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        builds.append(
            {
                "id": meta["id"],
                "env": env,
                "name": meta["name"],
                "channel": meta["channel"],
                "description": meta["description"],
                "version": version + meta["versionSuffix"],
                "path": f"firmware/{meta['file']}",
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "requiresAck": meta["requiresAck"],
            }
        )

    manifest = {
        "schema": 1,
        "repository": os.environ.get("GITHUB_REPOSITORY", "jtvargas/crossx-firmware"),
        "commit": git_commit(),
        "generatedAt": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "builds": builds,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Wrote {out} with {len(builds)} build(s): {', '.join(b['env'] for b in builds) or 'none'}")


if __name__ == "__main__":
    main()
