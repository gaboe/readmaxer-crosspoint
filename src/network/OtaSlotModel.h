#pragma once

#include <cstddef>
#include <cstdint>

// Byte model of the otadata partition and of an app slot header, free of
// ESP-IDF calls so the host tests can drive it. OtaBootSwitch.cpp applies it
// to flash. Layout reference: esp_flash_partitions.h and esp_app_format.h.

namespace ota_boot {

struct __attribute__((packed)) SelectEntry {
  uint32_t ota_seq;
  uint8_t seq_label[20];
  uint32_t ota_state;
  uint32_t crc;
};
static_assert(sizeof(SelectEntry) == 32, "SelectEntry must be 32 bytes");

constexpr uint32_t kOtaImgNew = 0;      // ESP_OTA_IMG_NEW
constexpr uint32_t kOtaImgValid = 2;    // ESP_OTA_IMG_VALID
constexpr uint32_t kOtaImgInvalid = 3;  // ESP_OTA_IMG_INVALID
constexpr uint32_t kOtaImgAborted = 4;  // ESP_OTA_IMG_ABORTED
constexpr uint32_t kSlotCount = 2;      // partitions.csv has ota_0 and ota_1
constexpr uint32_t kEntrySectors = 2;   // otadata holds one entry per 4 KiB sector

// CRC32-LE over the 4-byte ota_seq with init UINT32_MAX, as esp_rom_crc32_le
// and IDF otatool.py compute it.
uint32_t computeSeqCrc(uint32_t seq);

// Index of the entry the bootloader selects (valid CRC, not INVALID/ABORTED,
// highest seq, sector 0 on a tie), or -1 when neither entry is usable.
int activeEntry(const SelectEntry (&entries)[kEntrySectors]);

// The app slot an entry selects.
uint32_t entrySlot(const SelectEntry& entry);

struct EntryWrite {
  uint32_t sector;
  SelectEntry entry;
};

// The entry that makes the bootloader select `slot` with `state`: the smallest
// seq above the active one that maps to `slot`, written over the sector that
// does not hold the active entry, so a torn write leaves the active entry intact.
EntryWrite planSelect(const SelectEntry (&entries)[kEntrySectors], uint32_t slot, uint32_t state);

enum class SlotCheck {
  Confirmed,    // the active entry selects the running slot as VALID
  Unconfirmed,  // blank otadata, or the running slot's entry is not VALID yet
  FellBack,     // the active entry selects the other slot: the bootloader could not load it
};

SlotCheck checkRunningSlot(const SelectEntry (&entries)[kEntrySectors], uint32_t runningSlot);

struct BootPlan {
  bool confirmAfterHome;  // select the running slot as VALID once Home has rendered
  bool selectOtherSlot;   // select the other slot now, so a crash before Home boots it
};

// The fallback decision for one boot. The other slot is selected as VALID
// rather than NEW: a bootloader with rollback on aborts a NEW entry whose image
// never marks itself valid, and VALID boots under either rollback setting.
BootPlan planBoot(const SelectEntry (&entries)[kEntrySectors], uint32_t runningSlot, bool deepSleepWake,
                  bool otherSlotHoldsImage);

// Reads `len` bytes at `offset` of a slot. Returns false on a read error.
using SlotReader = bool (*)(uint32_t offset, void* dst, uint32_t len, void* ctx);

// Header-level check that a slot holds an app image for `chipId`: magic,
// segment count, chip id, and a segment table plus padding and SHA trailer that
// fit inside `slotSize`. It does not hash the segments; the bootloader does.
bool slotHoldsAppImage(SlotReader read, void* ctx, uint32_t slotSize, uint16_t chipId);

}  // namespace ota_boot
