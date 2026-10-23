#include "OtaSlotModel.h"

#include <cstring>

namespace ota_boot {

namespace {
constexpr uint32_t kBlankSeq = 0xFFFFFFFFu;
constexpr uint8_t kImageMagic = 0xE9;
constexpr uint32_t kImageHeaderSize = 24;
constexpr uint32_t kSegmentHeaderSize = 8;
constexpr uint8_t kMaxSegments = 16;  // ESP_IMAGE_MAX_SEGMENTS
constexpr uint32_t kShaTrailer = 32;
constexpr uint8_t kChecksumSeed = 0xEF;  // ESP_ROM_CHECKSUM_INITIAL

// ESP32-S3 flash-mapped data and instruction buses (soc.h SOC_DROM_*, SOC_IROM_*).
// The app descriptor opens the first segment, so it is always flash-mapped.
bool flashMapped(uint32_t address) {
  return (address >= 0x3C000000u && address < 0x3E000000u) || (address >= 0x42000000u && address < 0x44000000u);
}

bool entryUsable(const SelectEntry& e) {
  return e.ota_seq != kBlankSeq && e.crc == computeSeqCrc(e.ota_seq) && e.ota_state != kOtaImgInvalid &&
         e.ota_state != kOtaImgAborted;
}
}  // namespace

uint32_t computeSeqCrc(uint32_t seq) {
  uint8_t bytes[sizeof(seq)];
  std::memcpy(bytes, &seq, sizeof(seq));
  uint32_t crc = 0;  // ~UINT32_MAX: esp_rom_crc32_le inverts the init value
  for (const uint8_t b : bytes) {
    crc ^= b;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

int activeEntry(const SelectEntry (&entries)[kEntrySectors]) {
  int active = -1;
  for (int i = 0; i < static_cast<int>(kEntrySectors); ++i) {
    if (!entryUsable(entries[i])) continue;
    if (active < 0 || entries[i].ota_seq > entries[active].ota_seq) active = i;
  }
  return active;
}

uint32_t entrySlot(const SelectEntry& entry) { return (entry.ota_seq - 1u) % kSlotCount; }

EntryWrite planSelect(const SelectEntry (&entries)[kEntrySectors], uint32_t slot, uint32_t state) {
  const int active = activeEntry(entries);
  const uint32_t activeSeq = active < 0 ? 0 : entries[active].ota_seq;
  uint32_t seq = activeSeq + 1u;
  while ((seq - 1u) % kSlotCount != slot % kSlotCount) ++seq;

  EntryWrite write{};
  write.sector = active == 0 ? 1u : 0u;
  write.entry.ota_seq = seq;
  std::memset(write.entry.seq_label, 0xFF, sizeof(write.entry.seq_label));
  write.entry.ota_state = state;
  write.entry.crc = computeSeqCrc(seq);
  return write;
}

SlotCheck checkRunningSlot(const SelectEntry (&entries)[kEntrySectors], uint32_t runningSlot) {
  const int active = activeEntry(entries);
  if (active < 0) return SlotCheck::Unconfirmed;
  if (entrySlot(entries[active]) != runningSlot) return SlotCheck::FellBack;
  return entries[active].ota_state == kOtaImgValid ? SlotCheck::Confirmed : SlotCheck::Unconfirmed;
}

BootPlan planBoot(const SelectEntry (&entries)[kEntrySectors], uint32_t runningSlot, bool deepSleepWake) {
  const SlotCheck check = checkRunningSlot(entries, runningSlot);
  return BootPlan{check != SlotCheck::Confirmed, check == SlotCheck::Unconfirmed && !deepSleepWake};
}

bool planKeepRunningSlot(const SelectEntry (&entries)[kEntrySectors], uint32_t runningSlot, EntryWrite& write) {
  const int active = activeEntry(entries);
  if (active < 0 || entrySlot(entries[active]) != runningSlot) {
    write = planSelect(entries, runningSlot, kOtaImgNew);
    return true;
  }
  const uint32_t state = entries[active].ota_state;
  if (state == kOtaImgNew || state == kOtaImgValid) return false;
  write.sector = static_cast<uint32_t>(active);
  write.entry = entries[active];
  write.entry.ota_state = kOtaImgNew;
  return true;
}

bool slotHoldsAppImage(SlotReader read, void* ctx, uint32_t slotSize, uint16_t chipId, const ImageHasher& hasher,
                       uint8_t* buf, uint32_t bufLen) {
  uint8_t header[kImageHeaderSize];
  if (slotSize < kImageHeaderSize || bufLen < 16 || !read(0, header, kImageHeaderSize, ctx)) return false;
  const uint8_t segmentCount = header[1];
  uint16_t imageChip;
  std::memcpy(&imageChip, header + 12, sizeof(imageChip));
  if (header[0] != kImageMagic || segmentCount == 0 || segmentCount > kMaxSegments || imageChip != chipId ||
      !hasher.update(hasher.ctx, header, kImageHeaderSize)) {
    return false;
  }

  uint8_t checksum = kChecksumSeed;
  uint32_t pos = kImageHeaderSize;
  for (uint8_t i = 0; i < segmentCount; ++i) {
    uint8_t segmentHeader[kSegmentHeaderSize];
    if (slotSize - pos < kSegmentHeaderSize || !read(pos, segmentHeader, kSegmentHeaderSize, ctx) ||
        !hasher.update(hasher.ctx, segmentHeader, kSegmentHeaderSize)) {
      return false;
    }
    pos += kSegmentHeaderSize;
    uint32_t loadAddress;
    uint32_t dataLen;
    std::memcpy(&loadAddress, segmentHeader, sizeof(loadAddress));
    std::memcpy(&dataLen, segmentHeader + 4, sizeof(dataLen));
    if ((i == 0 && !flashMapped(loadAddress)) || dataLen % 4 != 0 || dataLen > slotSize - pos) return false;
    for (uint32_t left = dataLen; left > 0;) {
      const uint32_t chunk = left < bufLen ? left : bufLen;
      if (!read(pos, buf, chunk, ctx) || !hasher.update(hasher.ctx, buf, chunk)) return false;
      for (uint32_t b = 0; b < chunk; ++b) checksum ^= buf[b];
      pos += chunk;
      left -= chunk;
    }
  }

  // The checksum byte ends the 16-byte aligned padding, as in validateImageFile.
  if (slotSize - pos < 16) return false;
  const uint32_t padEnd = (pos + 16u) & ~15u;
  const uint32_t padLen = padEnd - pos;
  // A fallback target must carry the SHA-256 trailer that IDF builds append.
  if (header[23] == 0 || !read(pos, buf, padLen, ctx) || buf[padLen - 1] != checksum ||
      !hasher.update(hasher.ctx, buf, padLen)) {
    return false;
  }

  uint8_t stored[kShaTrailer];
  uint8_t computed[kShaTrailer];
  return kShaTrailer <= slotSize - padEnd && read(padEnd, stored, kShaTrailer, ctx) &&
         hasher.finish(hasher.ctx, computed) && std::memcmp(stored, computed, kShaTrailer) == 0;
}

}  // namespace ota_boot
