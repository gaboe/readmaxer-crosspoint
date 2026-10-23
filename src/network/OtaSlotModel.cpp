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
constexpr uint8_t kChecksumSeed = 0xEF;      // ESP_ROM_CHECKSUM_INITIAL
constexpr uint32_t kMmuPageSize = 0x10000u;  // CONFIG_MMU_PAGE_SIZE on X4 Pro

// ESP32-S3 flash-mapped data and instruction buses (soc.h SOC_DROM_*, SOC_IROM_*).
// The app descriptor opens the first segment, so it is always flash-mapped.
bool flashMapped(uint32_t address) {
  return (address >= 0x3C000000u && address < 0x3E000000u) || (address >= 0x42000000u && address < 0x44000000u);
}

uint16_t read16(const uint8_t* bytes) {
  uint16_t value;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

bool supportsRevisionBound(uint16_t maximum, uint16_t bound) { return maximum == 0 || maximum >= bound; }

bool inside(uint32_t address, uint32_t length, uint32_t low, uint32_t high) {
  return address >= low && address < high && length <= high - address;
}

bool s3LoadRange(uint32_t address, uint32_t length) {
  if (length == 0 || address < 0x10000000u) return true;
  if (flashMapped(address)) {
    return inside(address, length, 0x3C000000u, 0x3E000000u) || inside(address, length, 0x42000000u, 0x44000000u);
  }
  // IDF's S3 linker reserves everything above 0x403CB700 for the
  // loader, its DRAM alias, static data and stack. Exclude the whole area.
  return address % 4 == 0 &&
         (inside(address, length, 0x3FC88000u, 0x3FCDB700u) || inside(address, length, 0x40370000u, 0x403CB700u) ||
          inside(address, length, 0x600FE000u, 0x60100000u) || inside(address, length, 0x50000000u, 0x50002000u));
}

bool entryUsable(const SelectEntry& e) {
  return e.ota_seq != kBlankSeq && e.crc == computeSeqCrc(e.ota_seq) && e.ota_state != kOtaImgInvalid &&
         e.ota_state != kOtaImgAborted;
}

bool hasValidBootableImage(const SelectEntry (&entries)[kEntrySectors], const BootableSlots& images) {
  for (const auto& entry : entries) {
    if (entryUsable(entry) && entry.ota_state == kOtaImgValid && images.slots[entrySlot(entry)]) return true;
  }
  return false;
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

bool appEraseIsSafe(const SelectEntry (&entries)[kEntrySectors], const BootableSlots& images, uint32_t destination) {
  if (destination >= kSlotCount) return false;
  auto remaining = images;
  remaining.slots[destination] = false;
  return hasValidBootableImage(entries, remaining);
}

bool writeIsSafe(const SelectEntry (&entries)[kEntrySectors], const BootableSlots& images, const EntryWrite& write,
                 uint32_t intendedSlot, uint32_t intendedState, WritePolicy policy) {
  if (write.sector >= kEntrySectors || intendedSlot >= kSlotCount ||
      (intendedState != kOtaImgNew && intendedState != kOtaImgValid))
    return false;
  SelectEntry result[kEntrySectors];
  std::memcpy(result, entries, sizeof(result));
  std::memset(&result[write.sector], 0xFF, sizeof(SelectEntry));
  if (policy == WritePolicy::PreserveBootable && !hasValidBootableImage(result, images)) return false;
  result[write.sector] = write.entry;
  if (policy == WritePolicy::PreserveBootable && !hasValidBootableImage(result, images)) return false;
  // Rollback aborts every PENDING_VERIFY entry before selecting the highest sequence.
  for (auto& entry : result) {
    if (entry.ota_state == kOtaImgPendingVerify) entry.ota_state = kOtaImgAborted;
  }
  const int selected = activeEntry(result);
  return selected >= 0 && entrySlot(result[selected]) == intendedSlot && result[selected].ota_state == intendedState &&
         selected == static_cast<int>(write.sector);
}

bool planSelect(const SelectEntry (&entries)[kEntrySectors], const BootableSlots& images, uint32_t slot, uint32_t state,
                EntryWrite& write, WritePolicy policy) {
  if (slot >= kSlotCount) return false;
  const int active = activeEntry(entries);
  const uint32_t activeSeq = active < 0 ? 0 : entries[active].ota_seq;
  const uint32_t step = active < 0 ? slot + 1u : (entrySlot(entries[active]) == slot ? 2u : 1u);
  if (activeSeq > kBlankSeq - 1u - step) return false;
  EntryWrite candidate{};
  candidate.entry.ota_seq = activeSeq + step;
  std::memset(candidate.entry.seq_label, 0xFF, sizeof(candidate.entry.seq_label));
  candidate.entry.ota_state = state;
  candidate.entry.crc = computeSeqCrc(candidate.entry.ota_seq);
  for (uint32_t i = 0; i < kEntrySectors; ++i) {
    candidate.sector = (active == 0 ? 1u : 0u) ^ i;
    if (!writeIsSafe(entries, images, candidate, slot, state, policy)) continue;
    write = candidate;
    return true;
  }
  return false;
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

KeepPlan planKeepRunningSlot(const SelectEntry (&entries)[kEntrySectors], const BootableSlots& images,
                             uint32_t runningSlot, EntryWrite& write) {
  if (runningSlot >= kSlotCount) return KeepPlan::Unsafe;
  const int active = activeEntry(entries);
  if (active < 0 || entrySlot(entries[active]) != runningSlot) {
    return planSelect(entries, images, runningSlot, kOtaImgNew, write) ? KeepPlan::Write : KeepPlan::Unsafe;
  }
  const uint32_t state = entries[active].ota_state;
  if (state == kOtaImgNew || state == kOtaImgValid) return KeepPlan::NoWrite;
  EntryWrite next{static_cast<uint32_t>(active), entries[active]};
  next.entry.ota_state = kOtaImgNew;
  if (!writeIsSafe(entries, images, next, runningSlot, kOtaImgNew)) return KeepPlan::Unsafe;
  write = next;
  return KeepPlan::Write;
}

ImageCheck checkAppImage(SlotReader read, void* ctx, uint32_t slotSize, uint16_t chipId, const ImageHasher& hasher,
                         uint8_t* buf, uint32_t bufLen, SegmentVisitor visitor, void* visitorCtx, uint32_t* imageSize) {
  uint8_t header[kImageHeaderSize];
  if (slotSize > 0x1000000u || slotSize < kImageHeaderSize || bufLen < 16) return ImageCheck::BadSegments;
  if (!read(0, header, sizeof(header), ctx)) return ImageCheck::ReadError;
  if (header[0] != kImageMagic) return ImageCheck::BadMagic;
  if (read16(header + 12) != chipId) return ImageCheck::BadChip;
  const bool s3 = chipId == 9;
  if (s3 && (read16(header + 15) != 0 || !supportsRevisionBound(read16(header + 17), 99))) return ImageCheck::BadChip;
  const uint8_t segmentCount = header[1];
  if (segmentCount == 0 || segmentCount > kMaxSegments) return ImageCheck::BadSegments;
  if (!hasher.update(hasher.ctx, header, sizeof(header))) return ImageCheck::BadSha;
  if (visitor) visitor(header, sizeof(header), visitorCtx);

  uint8_t checksum = kChecksumSeed;
  uint32_t pos = kImageHeaderSize;
  bool hasDrom = false;
  bool hasIrom = false;
  for (uint8_t i = 0; i < segmentCount; ++i) {
    uint8_t segmentHeader[kSegmentHeaderSize];
    if (slotSize - pos < kSegmentHeaderSize) return ImageCheck::BadSegments;
    if (!read(pos, segmentHeader, sizeof(segmentHeader), ctx)) return ImageCheck::ReadError;
    if (!hasher.update(hasher.ctx, segmentHeader, sizeof(segmentHeader))) return ImageCheck::BadSha;
    if (visitor) visitor(segmentHeader, sizeof(segmentHeader), visitorCtx);
    pos += kSegmentHeaderSize;
    uint32_t loadAddress;
    uint32_t dataLen;
    std::memcpy(&loadAddress, segmentHeader, sizeof(loadAddress));
    std::memcpy(&dataLen, segmentHeader + 4, sizeof(dataLen));
    if ((i == 0 && !flashMapped(loadAddress)) || dataLen % 4 != 0 || dataLen >= 0x1000000u ||
        dataLen > slotSize - pos || (flashMapped(loadAddress) && loadAddress % kMmuPageSize != pos % kMmuPageSize) ||
        (s3 && !s3LoadRange(loadAddress, dataLen)))
      return ImageCheck::BadSegments;
    if (s3 && flashMapped(loadAddress)) {
      if (dataLen == 0) return ImageCheck::BadSegments;
      if (loadAddress < 0x3E000000u)
        hasDrom = true;
      else
        hasIrom = true;
    }
    if (s3 && i == 0) {
      if (dataLen < 256) return ImageCheck::BadSegments;
      if (!read(pos, buf, 4, ctx)) return ImageCheck::ReadError;
      uint32_t magic;
      std::memcpy(&magic, buf, sizeof(magic));
      if (magic != 0xABCD5432u) return ImageCheck::BadSegments;
      if (!read(pos + 176, buf, 8, ctx)) return ImageCheck::ReadError;
      const uint16_t minBlock = read16(buf);
      if ((buf[4] != 0 && buf[4] != 16) || (minBlock != 0 && minBlock != 0xFFFF) ||
          !supportsRevisionBound(read16(buf + 2), 199))
        return ImageCheck::BadSegments;
    }
    for (uint32_t left = dataLen; left > 0;) {
      const uint32_t chunk = left < bufLen ? left : bufLen;
      if (!read(pos, buf, chunk, ctx)) return ImageCheck::ReadError;
      if (!hasher.update(hasher.ctx, buf, chunk)) return ImageCheck::BadSha;
      if (visitor) visitor(buf, chunk, visitorCtx);
      for (uint32_t b = 0; b < chunk; ++b) checksum ^= buf[b];
      pos += chunk;
      left -= chunk;
    }
  }
  if (s3 && (!hasDrom || !hasIrom)) return ImageCheck::BadSegments;
  const uint32_t padEnd = (pos + 16u) & ~15u;
  const uint32_t padLen = padEnd - pos;
  if (padLen > slotSize - pos) return ImageCheck::BadSegments;
  if (!read(pos, buf, padLen, ctx)) return ImageCheck::ReadError;
  if (buf[padLen - 1] != checksum) return ImageCheck::BadChecksum;
  if (!hasher.update(hasher.ctx, buf, padLen)) return ImageCheck::BadSha;
  if (visitor) visitor(buf, padLen, visitorCtx);
  if (header[23] == 0) return ImageCheck::BadSha;
  if (kShaTrailer > slotSize - padEnd) return ImageCheck::BadSegments;
  uint8_t stored[kShaTrailer];
  uint8_t computed[kShaTrailer];
  if (!read(padEnd, stored, sizeof(stored), ctx)) return ImageCheck::ReadError;
  if (!hasher.finish(hasher.ctx, computed) || std::memcmp(stored, computed, sizeof(stored)) != 0)
    return ImageCheck::BadSha;
  if (visitor) visitor(stored, sizeof(stored), visitorCtx);
  if (imageSize) *imageSize = padEnd + kShaTrailer;
  return ImageCheck::Ok;
}

}  // namespace ota_boot
