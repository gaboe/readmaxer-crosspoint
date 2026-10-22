#pragma once

#include <esp_partition.h>

#include <cstdint>

#include "OtaSlotModel.h"

// X4 (and X3) factory bootloaders accept our patch_firmware_image.py-patched
// firmware.bin (web flasher proves this), but the running ESP-IDF's
// esp_image_verify rejects with bogus efuse-blk-rev errors. The SD-card update
// path bypasses that runtime check by writing the OTA app partition raw and
// updating otadata directly — same scheme as the web flasher
// (crosspoint-reader-docs/src/lib/flasher/OtaPartition.ts).

namespace ota_boot {

// Switch the bootloader's selected app partition to `dest` by writing a fresh
// otadata entry with `state` into the inactive otadata sector. Bypasses
// esp_ota_set_boot_partition's esp_image_verify call. The bytes in `dest` must
// already be a valid app image — caller is responsible for that.
//
// Returns true on success.
bool switchTo(const esp_partition_t* dest, uint32_t state = kOtaImgNew);

// Boot-loop fallback. Call first in setup(), after the power latch. While the
// running slot is not selected as VALID, it selects the other slot (when that
// slot holds an app image), so a crash before Home boots the other slot. Skipped
// on a deep-sleep wake. Does not log: Serial is not up yet.
void armBootFallback();

// Selects the running slot as VALID again and then cancels IDF rollback. Call
// once Home has rendered, and before overwriting the other slot. Returns false
// when the otadata write failed; the next call retries.
bool confirmRunningSlot();

// True from armBootFallback() until confirmRunningSlot() succeeds.
bool confirmationPending();

}  // namespace ota_boot
