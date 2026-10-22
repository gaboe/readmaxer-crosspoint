import hashlib
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from check_slot_image import ImageError, MAX_IMAGE_SIZE, validate_image


def make_image(chip_id=9, size=65_568, load_address=0x3C000020):
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = 1
    struct.pack_into("<H", header, 12, chip_id)
    header[23] = 1
    payload_size = size - 32 - 8 - 4 - 32
    segment = struct.pack("<II", load_address, payload_size) + bytes(payload_size)
    body = bytes(header) + segment
    pad_end = (len(body) + 16) & ~15
    padding = bytearray(pad_end - len(body))
    padding[-1] = 0xEF
    signed = body + padding
    return signed + hashlib.sha256(signed).digest()


class CheckSlotImageTests(unittest.TestCase):
    def test_good_app_image(self):
        validate_image(make_image())

    def test_truncated_segment_is_rejected(self):
        truncated = bytearray(make_image())
        struct.pack_into("<I", truncated, 28, len(truncated))
        with self.assertRaisesRegex(ImageError, "segment 0 data exceeds"):
            validate_image(bytes(truncated))

    def test_wrong_chip_is_rejected(self):
        with self.assertRaisesRegex(ImageError, "wrong chip id"):
            validate_image(make_image(chip_id=2))

    def test_bootloader_image_is_rejected(self):
        with self.assertRaisesRegex(ImageError, "bootloader"):
            validate_image(make_image(load_address=0x3FCE2820))

    def test_merged_image_partition_table_is_rejected(self):
        merged = bytearray(make_image())
        struct.pack_into("<HBBII", merged, 0x8000, 0x50AA, 1, 0, 0x10000, 0x400000)
        struct.pack_into("<HBBII", merged, 0x8020, 0x50AA, 1, 1, 0x410000, 0x400000)
        with self.assertRaisesRegex(ImageError, "partition table"):
            validate_image(bytes(merged))

    def test_oversized_image_is_rejected(self):
        with self.assertRaisesRegex(ImageError, "image too large"):
            validate_image(bytes(MAX_IMAGE_SIZE + 1))


if __name__ == "__main__":
    unittest.main()
