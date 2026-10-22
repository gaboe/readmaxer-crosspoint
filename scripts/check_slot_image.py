#!/usr/bin/env python3
"""Validate an X4 Pro app-only ESP32-S3 image before it becomes an artifact."""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

MIN_IMAGE_SIZE = 64 * 1024
MAX_IMAGE_SIZE = 0x5C0000
ESP_IMAGE_MAGIC = 0xE9
ESP32S3_CHIP_ID = 9
APP_IROM_START = 0x3C000000
APP_IROM_END = 0x3E000000
HEADER_SIZE = 24
SEGMENT_HEADER_SIZE = 8
SHA_SIZE = 32
BOARD_TAG = b"CROSSPOINT-BOARD-V1:"
BOARD_NAME = b"x4pro"


class ImageError(ValueError):
    pass


def has_partition_table(data: bytes) -> bool:
    """Return True when the image contains partition-table entries at offset 0x8000."""
    table = data[0x8000 : 0x8000 + 2 * 32]
    if len(table) < 64:
        return False
    valid = 0
    for offset in (0, 32):
        magic, kind, subtype, address, size = struct.unpack_from("<HBBII", table, offset)
        if magic == 0x50AA and kind in (0, 1, 2) and address % 0x1000 == 0 and size > 0:
            valid += 1
    return valid == 2


def validate_image(data: bytes) -> None:
    if len(data) < MIN_IMAGE_SIZE:
        raise ImageError(f"image too small: {len(data)} bytes")
    if len(data) > MAX_IMAGE_SIZE:
        raise ImageError(f"image too large: {len(data)} bytes exceeds 0x{MAX_IMAGE_SIZE:x}")
    if has_partition_table(data):
        raise ImageError("merged image contains a partition table at 0x8000")

    header = data[:HEADER_SIZE]
    if len(header) != HEADER_SIZE:
        raise ImageError("truncated image header")
    if header[0] != ESP_IMAGE_MAGIC:
        raise ImageError(f"bad image magic: 0x{header[0]:02x}")
    segment_count = header[1]
    if segment_count == 0:
        raise ImageError("image has no segments")
    chip_id = struct.unpack_from("<H", header, 12)[0]
    if chip_id != ESP32S3_CHIP_ID:
        raise ImageError(f"wrong chip id: {chip_id}, expected {ESP32S3_CHIP_ID}")

    offset = HEADER_SIZE
    checksum = 0xEF
    digest = hashlib.sha256()
    digest.update(header)
    for index in range(segment_count):
        if offset + SEGMENT_HEADER_SIZE > len(data):
            raise ImageError(f"segment {index} header exceeds file size")
        segment_header = data[offset : offset + SEGMENT_HEADER_SIZE]
        load_address = struct.unpack_from("<I", segment_header)[0]
        if index == 0 and not APP_IROM_START <= load_address < APP_IROM_END:
            raise ImageError(f"image starts with a bootloader or non-app segment at 0x{load_address:08x}")
        digest.update(segment_header)
        length = struct.unpack_from("<I", segment_header, 4)[0]
        offset += SEGMENT_HEADER_SIZE
        end = offset + length
        if end > len(data):
            raise ImageError(f"segment {index} data exceeds file size")
        segment_data = data[offset:end]
        for byte in segment_data:
            checksum ^= byte
        digest.update(segment_data)
        offset = end

    pad_end = (offset + 16) & ~15
    expected_size = pad_end + (SHA_SIZE if header[23] else 0)
    if expected_size != len(data):
        raise ImageError(f"image size mismatch: expected {expected_size}, got {len(data)}")
    padding = data[offset:pad_end]
    if not padding or checksum != padding[-1]:
        raise ImageError("image checksum mismatch")
    digest.update(padding)

    if header[23]:
        if digest.digest() != data[pad_end:expected_size]:
            raise ImageError("image SHA-256 trailer mismatch")

    tag_start = data.find(BOARD_TAG)
    if tag_start >= 0:
        tag_end = data.find(b";", tag_start + len(BOARD_TAG))
        if tag_end >= 0 and data[tag_start + len(BOARD_TAG) : tag_end] != BOARD_NAME:
            raise ImageError("image board tag is not x4pro")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    try:
        data = args.image.read_bytes()
        validate_image(data)
    except (OSError, ImageError) as error:
        print(f"ERROR slot image: {error}", file=sys.stderr)
        return 1
    print(f"OK slot image ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
