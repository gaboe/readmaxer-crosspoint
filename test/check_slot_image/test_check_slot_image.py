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
    header[1] = len(segments) + 1
    struct.pack_into("<H", header, 12, chip)
    header[23] = 1 if sha else 0
    body = bytearray(header)
    checksum = 0xEF
    for i, data in enumerate(segments):
        address = first_addr + i * 0x10000 + ((len(body) + 8 - first_addr) % checker.MMU_PAGE_SIZE)
        body += struct.pack("<II", address, len(data)) + data
        for b in data:
            checksum ^= b
    body += struct.pack("<II", 0x42000000 + (len(body) + 8) % 65536, 4) + bytes(4)
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
        self.assertIn("bad segment count 18", refused(build_image(segments)))

    def test_first_segment_outside_flash_mapping_is_refused(self):
        image = build_image([APP_DESC + bytes(252), bytes(70_000)], first_addr=0x40374000)
        self.assertIn("not flash-mapped", refused(image))

    def test_image_without_sha_trailer_is_refused(self):
        image = build_image([APP_DESC + bytes(252), bytes(70_000)], sha=False)
        self.assertIn("no SHA-256 trailer", refused(image))

    def test_unaligned_segment_length_is_refused(self):
        image = build_image([APP_DESC + bytes(252), bytes(70_001)])
        self.assertIn("not 4-byte aligned", refused(image))

    def test_mapped_segment_mmu_offset_is_checked(self):
        good = app_image()
        self.assertEqual(checker.check(good)["segments"], 3)
        bad = bytearray(good)
        struct.pack_into("<I", bad, 24, struct.unpack_from("<I", bad, 24)[0] + 4)
        self.assertIn("MMU page offset", refused(bytes(bad)))

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
        self.assertEqual(checker.check(image)["segments"], 3)

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

    def test_revision_and_descriptor_rules(self):
        good = app_image()
        cases = [
            (15, "H", 0, True), (15, "H", 1, False),
            (17, "H", 99, True), (17, "H", 98, False), (17, "H", 65535, True),
            (208, "H", 0, True), (208, "H", 1, False), (208, "H", 65535, True),
            (210, "H", 199, True), (210, "H", 198, False), (210, "H", 65535, True),
            (212, "B", 0, True), (212, "B", 16, True), (212, "B", 15, False), (212, "B", 32, False),
            (32, "I", 0, False), (1, "B", 0, False), (1, "B", 17, False),
            (28, "I", 252, False), (28, "I", 0x1000000, False),
        ]
        for offset, fmt, value, accepted in cases:
            with self.subTest(offset=offset, value=value):
                data = bytearray(good)
                struct.pack_into("<" + fmt, data, offset, value)
                checksum = 0xef
                for byte in data[32:288] + data[296:296 + (200_000 // 256) * 256]:
                    checksum ^= byte
                data[-33] = checksum
                data[-32:] = hashlib.sha256(data[:-32]).digest()
                if accepted:
                    self.assertEqual(checker.check(data)["segments"], 3)
                else:
                    refused(data)

    def test_load_address_ranges(self):
        first = APP_DESC + bytes(252)
        # Keep the fixture above the release minimum without enlarging the RAM segment.
        good = build_image([first + bytes(65536), bytes(64)])
        second_header = 24 + 8 + len(first) + 65536
        cases = [
            (0, True), (4, True), (0x3FC88000, True), (0x3FCDB6C0, True),
            (0x3FCDB6C4, False), (0x3FCDB700, False), (0x3FCE2700, False),
            (0x3FCE9700, False), (0x40370000, True), (0x403CB6C0, True),
            (0x403CB6C4, False), (0x403CB700, False), (0x403D2700, False),
            (0x600FE000, True), (0x600FFFC0, True), (0x600FFFC4, False),
            (0x50000000, True), (0x50001FC0, True), (0x50001FC4, False),
            (0x60000000, False), (0x3FC87FFC, False), (0xFFFFFFFC, False),
            (0x3FC88001, False), (0x42000128, True), (0x4200012C, False),
        ]
        for address, accepted in cases:
            with self.subTest(address=hex(address)):
                data = bytearray(good)
                struct.pack_into("<I", data, second_header, address)
                data[-32:] = hashlib.sha256(data[:-32]).digest()
                if accepted:
                    self.assertEqual(checker.check(data)["segments"], 3)
                else:
                    refused(data)

    def test_mapping_presence_and_extent(self):
        good = build_image([APP_DESC + bytes(252), bytes(65536)])
        last_header = 296 + 65536
        for offset, value, accepted in [(24, 0x42000020, False),
                                       (last_header, 0x40374000, False),
                                       (288, 0x43FE0128, True), (288, 0x43FF0128, False)]:
            with self.subTest(offset=offset, value=hex(value)):
                data = bytearray(good)
                struct.pack_into("<I", data, offset, value)
                if offset == 24:
                    struct.pack_into("<I", data, 288, 0x42000128)
                data[-32:] = hashlib.sha256(data[:-32]).digest()
                if accepted:
                    checker.check(data)
                else:
                    refused(data)

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
