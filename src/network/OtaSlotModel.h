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

constexpr uint32_t kOtaImgNew = 0;            // ESP_OTA_IMG_NEW
constexpr uint32_t kOtaImgPendingVerify = 1;  // ESP_OTA_IMG_PENDING_VERIFY
constexpr uint32_t kOtaImgValid = 2;          // ESP_OTA_IMG_VALID
constexpr uint32_t kOtaImgInvalid = 3;        // ESP_OTA_IMG_INVALID
constexpr uint32_t kOtaImgAborted = 4;        // ESP_OTA_IMG_ABORTED
constexpr uint32_t kSlotCount = 2;            // partitions.csv has ota_0 and ota_1
constexpr uint32_t kEntrySectors = 2;         // otadata holds one entry per 4 KiB sector

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

// Current checksum/SHA-verified images without a guest marker. Host markers are irrelevant.
struct BootableSlots {
  bool slots[kSlotCount]{};
};

enum class SelectionResult { Selected, Failed, Unknown };
enum class WritePolicy { PreserveBootable, Legacy };

bool appEraseIsSafe(const SelectEntry (&entries)[kEntrySectors], const BootableSlots& images, uint32_t destination);

// Check the erased and completed states before any sector is touched.
bool writeIsSafe(const SelectEntry (&entries)[kEntrySectors], const BootableSlots& images, const EntryWrite& write,
                 uint32_t intendedSlot, uint32_t intendedState, WritePolicy policy = WritePolicy::PreserveBootable);

bool planSelect(const SelectEntry (&entries)[kEntrySectors], const BootableSlots& images, uint32_t slot, uint32_t state,
                EntryWrite& write, WritePolicy policy = WritePolicy::PreserveBootable);

enum class SlotCheck {
  Confirmed,    // the active entry selects the running slot as VALID
  Unconfirmed,  // blank otadata, or the running slot's entry is not VALID yet
  FellBack,     // the active entry selects the other slot: the bootloader could not load it
};

SlotCheck checkRunningSlot(const SelectEntry (&entries)[kEntrySectors], uint32_t runningSlot);

struct BootPlan {
  bool confirmAfterHome;  // select the running slot as VALID once Home has rendered
  bool armFallback;       // select the other slot now if it holds a verified image, so a crash before Home boots it
};

// The fallback decision for one boot. The other slot is selected as VALID
// rather than NEW: a bootloader with rollback on aborts a NEW entry whose image
// never marks itself valid, and VALID boots under either rollback setting.
BootPlan planBoot(const SelectEntry (&entries)[kEntrySectors], uint32_t runningSlot, bool deepSleepWake);

// Preserve a VALID entry for a bootable non-guest image while arranging a deliberate wake into the running slot.
enum class KeepPlan { NoWrite, Write, Unsafe };
KeepPlan planKeepRunningSlot(const SelectEntry (&entries)[kEntrySectors], const BootableSlots& images,
                             uint32_t runningSlot, EntryWrite& write);

// Reads `len` bytes at `offset` of a slot. Returns false on a read error.
using SlotReader = bool (*)(uint32_t offset, void* dst, uint32_t len, void* ctx);

// SHA-256 supplied by the caller (mbedtls on the device), so the host tests can
// drive the image check. The caller starts the hash before the check.
struct ImageHasher {
  void* ctx;
  bool (*update)(void* ctx, const uint8_t* data, uint32_t len);
  bool (*finish)(void* ctx, uint8_t (&digest)[32]);
};

using SegmentVisitor = void (*)(const uint8_t* data, uint32_t len, void* ctx);

enum class ImageCheck { Ok, ReadError, BadMagic, BadChip, BadSegments, BadChecksum, BadSha };

// Streaming image check shared by SD files and flash slots. S3 acceptance is
// bounded to chip revisions 0..99, eFuse block revisions 0..199, 64 KiB MMU
// pages and the IDF 5.4.3 application RAM region below the bootloader.
// imageSize excludes unused bytes at the end of a flash slot.
ImageCheck checkAppImage(SlotReader read, void* ctx, uint32_t slotSize, uint16_t chipId, const ImageHasher& hasher,
                         uint8_t* buf, uint32_t bufLen, SegmentVisitor visitor = nullptr, void* visitorCtx = nullptr,
                         uint32_t* imageSize = nullptr);

}  // namespace ota_boot
