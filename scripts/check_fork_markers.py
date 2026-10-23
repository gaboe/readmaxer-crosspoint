#!/usr/bin/env python3
"""Check the identity markers in a built X4 Pro app image."""

import sys
from pathlib import Path


def main() -> int:
    image = Path(sys.argv[1]).read_bytes()
    guest = image.count(b"READMAXER-GUEST-V1;")
    host = image.count(b"READMAXER-HOST-V1;")
    print(f"image={len(image)} guest_marker={guest} host_marker={host}")
    return 0 if guest == 0 and host == 1 else 1


if __name__ == "__main__":
    raise SystemExit(main())
