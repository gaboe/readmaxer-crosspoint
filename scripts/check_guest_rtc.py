"""Fail the X4 Pro build if firmware RTC storage overlaps the guest log."""

import re
from pathlib import Path

Import("env")  # noqa: F821

LIMIT = 0x50001800
SYMBOLS = ("_rtc_data_end", "_rtc_bss_end", "_rtc_noinit_end", "_rtc_force_slow_end")


def check_guest_rtc(target, source, env):
    map_path = Path(env.subst("$BUILD_DIR/${PROGNAME}.map"))
    content = map_path.read_text()
    for symbol in SYMBOLS:
        match = re.search(r"^\s*(0x[0-9a-fA-F]+)\s+" + symbol + r"\s*=", content, re.MULTILINE)
        if match is None:
            raise RuntimeError(f"RTC layout check cannot find {symbol} in {map_path}")
        end = int(match.group(1), 16)
        if end > LIMIT:
            raise RuntimeError(f"RTC layout overlaps guest log: {symbol}=0x{end:x} > 0x{LIMIT:x}")
    print(f"Guest RTC range 0x{LIMIT:x}..0x50002000 is clear of firmware RTC sections")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", check_guest_rtc)  # noqa: F821
