#!/usr/bin/env python3
"""
read-fw-version.py

Reads FW_VERSION_MAJOR / MINOR / PATCH from a C header at build time and
delegates to sign-firmware.py with the correct --version string.

Usage (as called by CMakeLists.txt):
  python read-fw-version.py <Firmware_Version.h>
          --sign <sign-firmware.py> <input.bin> <output.bin>
          --product mywota
          --max-total-size <bytes>
"""

import argparse
import re
import subprocess
import sys


def parse_version(header_path: str) -> str:
    with open(header_path, "r", encoding="utf-8") as f:
        text = f.read()

    components = {}
    for part in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(
            r"#define\s+FW_VERSION_" + part + r"\s+(\d+)", text
        )
        if not m:
            sys.exit(f"ERROR: Could not find FW_VERSION_{part} in {header_path}")
        components[part] = m.group(1)

    return f"{components['MAJOR']}.{components['MINOR']}.{components['PATCH']}"


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Read firmware version from header and invoke sign-firmware.py"
    )
    parser.add_argument("header", help="Path to Firmware_Version.h")
    parser.add_argument("--sign", nargs=3,
                        metavar=("SIGN_SCRIPT", "INPUT_BIN", "OUTPUT_BIN"),
                        required=True,
                        help="sign-firmware.py path, input .bin, output .bin")
    parser.add_argument("--product", required=True)
    parser.add_argument("--max-total-size", type=int, default=None)

    args = parser.parse_args()

    version = parse_version(args.header)
    sign_script, input_bin, output_bin = args.sign

    cmd = [
        sys.executable, sign_script,
        input_bin, output_bin,
        "--product", args.product,
        "--version", version,
    ]
    if args.max_total_size is not None:
        cmd += ["--max-total-size", str(args.max_total_size)]

    sys.exit(subprocess.call(cmd))


if __name__ == "__main__":
    main()
