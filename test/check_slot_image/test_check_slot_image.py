import contextlib
import hashlib
import importlib.util
import io
import struct
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "check_slot_image.py"
_spec = importlib.util.spec_from_file_location("slot_image_checker", SCRIPT)
checker = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(checker)

APP_DESC = struct.pack("<I", checker.APP_DESC_MAGIC)


def build_image(segments, chip=9, sha=True, first_addr=0x3C000000):
    """A well-formed ESP app image: header, segments, padding with checksum, SHA-256 trailer."""
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = len(segments)
    struct.pack_into("<H", header, 12, chip)
    header[23] = 1 if sha else 0
    body = bytearray(header)
    checksum = 0xEF
    for i, data in enumerate(segments):
        body += struct.pack("<II", first_addr + i * 0x10000, len(data)) + data
        for b in data:
            checksum ^= b
    pad_end = (len(body) + 16) & ~15
    body += bytes(pad_end - len(body))
    body[-1] = checksum
    if sha:
        body += hashlib.sha256(body).digest()
    return bytes(body)


def app_image(payload_size=200_000, chip=9, extra=b""):
    first = APP_DESC + bytes(252) + extra
    first += bytes(-len(first) % 4)
    return build_image([first, bytes(range(256)) * (payload_size // 256)], chip=chip)


def refused(data, **kwargs):
    try:
        checker.check(data, **kwargs)
    except checker.Refused as err:
        return str(err)
    raise AssertionError("image was accepted")


class CheckSlotImageTest(unittest.TestCase):
    def test_good_image_passes(self):
        info = checker.check(app_image())
        self.assertEqual(info["chip"], 9)
        self.assertTrue(info["sha_appended"])

    def test_segment_count_above_esp_image_max_is_refused(self):
        segments = [APP_DESC + bytes(252)] + [bytes(4096)] * 16
        self.assertIn("bad segment count 17", refused(build_image(segments)))

    def test_first_segment_outside_flash_mapping_is_refused(self):
        image = build_image([APP_DESC + bytes(252), bytes(70_000)], first_addr=0x40374000)
        self.assertIn("not flash-mapped", refused(image))

    def test_image_without_sha_trailer_is_refused(self):
        image = build_image([APP_DESC + bytes(252), bytes(70_000)], sha=False)
        self.assertIn("no SHA-256 trailer", refused(image))

    def test_unaligned_segment_length_is_refused(self):
        image = build_image([APP_DESC + bytes(252), bytes(70_001)])
        self.assertIn("not 4-byte aligned", refused(image))

    def test_truncated_image_is_refused(self):
        self.assertIn("overruns EOF", refused(app_image()[:100_000]))
        self.assertIn("size mismatch", refused(app_image()[:-1]))

    def test_wrong_chip_is_refused(self):
        self.assertIn("wrong chip", refused(app_image(chip=5)))

    def test_merged_factory_image_is_refused(self):
        bootloader = build_image([bytes(16_000)])
        table = bytearray(32)
        table[:2] = checker.PARTITION_ENTRY_MAGIC
        table[12:15] = b"nvs"
        merged = bytearray(b"\xff" * 0x10000)
        merged[: len(bootloader)] = bootloader
        merged[0x8000:0x8020] = table
        merged += app_image()
        self.assertIn("partition table at 0x8000", refused(bytes(merged)))

    def test_bootloader_at_zero_without_table_is_refused(self):
        bootloader = build_image([bytes(100_000)])
        self.assertIn("bootloader at 0x0", refused(bootloader))

    def test_partition_magic_inside_app_data_is_not_a_table(self):
        payload = bytearray(200_000)
        second_segment_start = 24 + 8 + 256 + 8
        payload[0x8000 - second_segment_start : 0x8000 - second_segment_start + 2] = checker.PARTITION_ENTRY_MAGIC
        image = build_image([APP_DESC + bytes(252), bytes(payload)])
        self.assertEqual(image[0x8000:0x8002], checker.PARTITION_ENTRY_MAGIC)
        self.assertEqual(checker.check(image)["segments"], 2)

    def test_oversized_image_is_refused(self):
        image = app_image(payload_size=checker.MAX_SLOT_IMAGE)
        self.assertGreater(len(image), checker.MAX_SLOT_IMAGE)
        self.assertIn("too large", refused(image))

    def test_corrupt_checksum_and_sha_are_refused(self):
        image = bytearray(app_image())
        image[300] ^= 0x01
        self.assertIn("checksum mismatch", refused(bytes(image)))
        image = bytearray(app_image())
        image[-1] ^= 0x01
        self.assertIn("SHA-256 mismatch", refused(bytes(image)))

    def test_board_tag_for_another_board_is_refused(self):
        tagged = app_image(extra=b"CROSSPOINT-BOARD-V1:x4;")
        self.assertIn("wrong board", refused(tagged, board="x4pro"))
        self.assertEqual(checker.check(app_image(extra=b"CROSSPOINT-BOARD-V1:x4pro;"), board="x4pro")["tags"], ["x4pro"])

    def test_cli_prints_ok_or_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            good = Path(tmp) / "firmware.bin"
            good.write_bytes(app_image())
            bad = Path(tmp) / "firmware.factory.bin"
            bad.write_bytes(app_image(chip=5))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(checker.main([str(good)]), 0)
                self.assertEqual(checker.main([str(bad)]), 1)
            lines = out.getvalue().splitlines()
            self.assertTrue(lines[0].startswith("OK slot image"))
            self.assertTrue(lines[1].startswith("REFUSED:"))


if __name__ == "__main__":
    unittest.main()
