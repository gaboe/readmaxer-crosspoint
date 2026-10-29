#include "CpBootRecord.h"

#include <algorithm>

namespace cp_boot {
namespace {
constexpr size_t Header = 8;
constexpr size_t BootBytes = 16;
uint16_t read16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t crc(const Record::Encoded& bytes) {
  uint32_t value = UINT32_MAX;
  for (size_t i = 0; i < Record::Bytes - 4; ++i) {
    value ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
  }
  return ~value;
}
}  // namespace

const char* stageName(Stage stage) {
  switch (stage) {
    case Stage::None:
      return "none";
    case Stage::SetupEnter:
      return "setup-enter";
    case Stage::StorageBegin:
      return "storage-begin";
    case Stage::HostInit:
      return "host-init";
    case Stage::SlotInit:
      return "slot-init";
    case Stage::CardInit:
      return "card-init";
    case Stage::SectorZero:
      return "sector-zero-read";
    case Stage::VolumeMount:
      return "volume-mount";
    case Stage::StorageEnd:
      return "storage-end";
    case Stage::StorageFailed:
      return "storage-failed";
    case Stage::PanelInit:
      return "panel-init";
    case Stage::FirstDraw:
      return "first-draw";
  }
  return "unknown";
}

void Record::seal() {
  const uint32_t value = crc(bytes);
  for (size_t i = 0; i < 4; ++i) bytes[Bytes - 4 + i] = value >> (8 * i);
}

bool Record::decode(const Encoded& source) {
  uint32_t saved = 0;
  for (size_t i = 0; i < 4; ++i) saved |= uint32_t(source[Bytes - 4 + i]) << (8 * i);
  if (source[0] != 'C' || source[1] != 'P' || source[2] != 'B' || source[3] != 'T' || source[4] != 1 ||
      source[5] > Boots || source[6] != 0 || source[7] != 0 || saved != crc(source)) {
    bytes = {};
    return false;
  }
  bytes = source;
  return true;
}

void Record::begin(uint8_t reason) {
  const uint16_t next = count() ? uint16_t(counter(0) + 1) : 1;
  std::copy_n(bytes.begin() + Header, BootBytes, bytes.begin() + Header + BootBytes);
  std::fill_n(bytes.begin() + Header, BootBytes, 0);
  bytes[0] = 'C';
  bytes[1] = 'P';
  bytes[2] = 'B';
  bytes[3] = 'T';
  bytes[4] = 1;
  bytes[5] = std::min<size_t>(count() + 1, Boots);
  bytes[Header] = next;
  bytes[Header + 1] = next >> 8;
  bytes[Header + 2] = reason;
  stage(Stage::SetupEnter);
}

void Record::stage(Stage value) {
  bytes[Header + 3] = static_cast<uint8_t>(value);
  seal();
}

void Record::attempt(size_t index, Stage value, int32_t error) {
  if (index >= Attempts) return;
  const int16_t compact = error < INT16_MIN + 2 || error > INT16_MAX ? ErrorOutOfRange : int16_t(error);
  const size_t offset = Header + 4 + index * 3;
  bytes[offset] = static_cast<uint8_t>(value);
  const uint16_t raw = static_cast<uint16_t>(error == Pending ? Pending : compact);
  bytes[offset + 1] = raw;
  bytes[offset + 2] = raw >> 8;
  stage(value);
}

uint16_t Record::counter(size_t boot) const { return read16(&bytes[Header + boot * BootBytes]); }
uint8_t Record::resetReason(size_t boot) const { return bytes[Header + boot * BootBytes + 2]; }
Stage Record::lastStage(size_t boot) const { return static_cast<Stage>(bytes[Header + boot * BootBytes + 3]); }
Attempt Record::sdAttempt(size_t boot, size_t index) const {
  const size_t offset = Header + boot * BootBytes + 4 + index * 3;
  const uint16_t raw = read16(&bytes[offset + 1]);
  const int16_t error = raw <= INT16_MAX ? int16_t(raw) : int16_t(int32_t(raw) - 65536);
  return {static_cast<Stage>(bytes[offset]), error};
}
}  // namespace cp_boot
