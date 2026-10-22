#pragma once

#include <esp_partition.h>

#include <cstddef>
#include <cstdint>

#include "OtaBootSwitchModel.h"

// X4 (and X3) factory bootloaders accept our patch_firmware_image.py-patched
// firmware.bin (web flasher proves this), but the running ESP-IDF's
// esp_image_verify rejects with bogus efuse-blk-rev errors. Both SD-card and
// OTA update paths bypass that runtime check by writing the OTA app partition
// raw and updating otadata directly — same scheme as the web flasher
// (crosspoint-reader-docs/src/lib/flasher/OtaPartition.ts).
//
// Layout reference: esp_flash_partitions.h. CRC covers ota_seq (4 bytes) only.

namespace ota_boot {

// Switch the bootloader's selected app partition to `dest` by writing a fresh
// otadata entry into the inactive otadata slot. Bypasses esp_ota_set_boot_partition's
// esp_image_verify call. The bytes in `dest` must already be a valid app image
// (e.g. patch_firmware_image.py output) — caller is responsible for that.
//
// Returns true on success.
bool switchTo(const esp_partition_t* dest, uint32_t state = kOtaImgNew);

// Read the newest CRC-valid otadata state for an OTA app partition.
bool entryState(const esp_partition_t* partition, uint32_t& state);

// Select the opposite OTA slot while an X4 Pro image is unconfirmed. This
// provides a fallback target if the running image resets before Home renders.
void selectFallbackForUnconfirmedX4Pro();

// Mark the running image VALID after the first Home frame has reached the display.
void confirmRunningX4ProAfterHome();

}  // namespace ota_boot
