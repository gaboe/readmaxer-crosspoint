#!/usr/bin/env python3
"""Refuse any file that is not a single app image safe to write into an OTA slot.

A Python port of firmware_flash::validateImageFile (src/network/FirmwareFlasher.cpp),
plus the segment-count, first-segment, segment-alignment, and required SHA-256
rules of ota_boot::slotHoldsAppImage, and two more rules for this fork: a merged image (bootloader at 0x0 or partition
table at 0x8000, such as firmware.factory.bin) is refused, and so is an image
larger than 0x5C0000, which keeps 0x80000 of headroom in a 0x640000 slot.

Usage: check_slot_image.py [--chip ID] [--board NAME] IMAGE
Prints "OK slot image ..." and exits 0, or prints "REFUSED: <reason>" and exits 1.
"""

import argparse
import hashlib
import struct
import sys

IMAGE_MAGIC = 0xE9
HEADER_SIZE = 24
SEGMENT_HEADER_SIZE = 8
SHA_TRAILER = 32
CHECKSUM_SEED = 0xEF
MIN_FIRMWARE_SIZE = 64 * 1024
MAX_SLOT_IMAGE = 0x5C0000
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_ENTRY_MAGIC = b"\xaa\x50"
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_OFFSET = HEADER_SIZE + SEGMENT_HEADER_SIZE
BOARD_TAG_MAGIC = b"CROSSPOINT-BOARD-V1:"
CHIP_ESP32S3 = 9
MAX_SEGMENTS = 16  # ESP_IMAGE_MAX_SEGMENTS


class Refused(Exception):
    pass


def _has_partition_table(data):
    """A partition table entry is magic, type, subtype, offset, size, and a printable label."""
    entry = data[PARTITION_TABLE_OFFSET : PARTITION_TABLE_OFFSET + 32]
    if len(entry) < 32 or entry[:2] != PARTITION_ENTRY_MAGIC:
        return False
    label = entry[12:28].rstrip(b"\x00")
    return 0 < len(label) and all(0x20 < b < 0x7F for b in label)


def _flash_mapped(addr):
    """ESP32-S3 flash-mapped DROM and IROM, as ota_boot::slotHoldsAppImage checks the first segment."""
    return 0x3C000000 <= addr < 0x3E000000 or 0x42000000 <= addr < 0x44000000


def _scan_board_tags(segment_data):
    names = []
    start = 0
    while True:
        at = segment_data.find(BOARD_TAG_MAGIC, start)
        if at < 0:
            return names
        end = segment_data.find(b";", at + len(BOARD_TAG_MAGIC), at + len(BOARD_TAG_MAGIC) + 24)
        name = segment_data[at + len(BOARD_TAG_MAGIC) : end] if end >= 0 else b""
        if name and all(0x20 < b < 0x7F for b in name):
            names.append(name.decode("ascii"))
        start = at + 1


def check(data, chip=CHIP_ESP32S3, board=None):
    """Return a summary dict for a good slot image, or raise Refused."""
    size = len(data)
    if size < MIN_FIRMWARE_SIZE:
        raise Refused(f"too small: {size} bytes")
    if _has_partition_table(data):
        raise Refused("merged image: partition table at 0x8000")
    if data[0] != IMAGE_MAGIC:
        raise Refused(f"bad magic 0x{data[0]:02X}")
    if size > MAX_SLOT_IMAGE:
        raise Refused(f"too large: {size} bytes > 0x{MAX_SLOT_IMAGE:X}")

    header = data[:HEADER_SIZE]
    segment_count = header[1]
    image_chip = struct.unpack_from("<H", header, 12)[0]
    hash_appended = header[23] != 0
    if image_chip != chip:
        raise Refused(f"wrong chip: image={image_chip} expected={chip}")
    if not 0 < segment_count <= MAX_SEGMENTS:
        raise Refused(f"bad segment count {segment_count}")
    if not hash_appended:
        raise Refused("no SHA-256 trailer")
    if size >= APP_DESC_OFFSET + 4 and struct.unpack_from("<I", data, APP_DESC_OFFSET)[0] != APP_DESC_MAGIC:
        raise Refused("no app descriptor after the first segment header: bootloader at 0x0")

    checksum = CHECKSUM_SEED
    pos = HEADER_SIZE
    segment_bytes = []
    for i in range(segment_count):
        if pos + SEGMENT_HEADER_SIZE > size:
            raise Refused(f"segment {i} header overruns EOF at {pos}")
        load_addr, data_len = struct.unpack_from("<II", data, pos)
        if i == 0 and not _flash_mapped(load_addr):
            raise Refused(f"first segment loads to 0x{load_addr:08X}, not flash-mapped DROM or IROM")
        if data_len % 4:
            raise Refused(f"segment {i} length {data_len} is not 4-byte aligned")
        pos += SEGMENT_HEADER_SIZE
        if pos + data_len > size:
            raise Refused(f"segment {i} data overruns EOF ({pos} + {data_len} > {size})")
        segment = data[pos : pos + data_len]
        segment_bytes.append(segment)
        for b in segment:
            checksum ^= b
        pos += data_len

    tags = _scan_board_tags(b"".join(segment_bytes))
    if board is not None and any(tag != board for tag in tags):
        raise Refused(f"wrong board: image={','.join(tags)} expected={board}")

    pad_end = (pos + 16) & ~15
    expected = pad_end + (SHA_TRAILER if hash_appended else 0)
    if expected != size:
        raise Refused(f"size mismatch: body+pad={pad_end} expected={expected} actual={size}")
    if data[pad_end - 1] != checksum:
        raise Refused(f"checksum mismatch: computed=0x{checksum:02X} stored=0x{data[pad_end - 1]:02X}")
    if hash_appended and hashlib.sha256(data[:pad_end]).digest() != data[pad_end:]:
        raise Refused("SHA-256 mismatch")

    return {
        "size": size,
        "segments": segment_count,
        "sha_appended": hash_appended,
        "chip": image_chip,
        "headroom": MAX_SLOT_IMAGE - size,
        "tags": tags,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image")
    parser.add_argument("--chip", type=int, default=CHIP_ESP32S3, help="expected chip_id (default 9, ESP32-S3)")
    parser.add_argument("--board", help="refuse a board tag naming another board, for example x4pro")
    args = parser.parse_args(argv)
    with open(args.image, "rb") as f:
        data = f.read()
    try:
        info = check(data, chip=args.chip, board=args.board)
    except Refused as err:
        print(f"REFUSED: {err}")
        return 1
    print(
        f"OK slot image size={info['size']} segments={info['segments']} sha_appended={info['sha_appended']} "
        f"chip={info['chip']} headroom={info['headroom']} sha256={hashlib.sha256(data).hexdigest()}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
