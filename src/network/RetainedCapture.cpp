#include "RetainedCapture.h"

#include <algorithm>
#include <cstring>

namespace guest_log {
namespace {
uint64_t little(const uint8_t* bytes, size_t length) {
  uint64_t value = 0;
  for (size_t i = 0; i < length; ++i) value |= uint64_t(bytes[i]) << (8 * i);
  return value;
}

uint32_t crc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xffffffff;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return ~crc;
}

bool zero(const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i)
    if (data[i] != 0) return false;
  return true;
}

bool committed(const uint8_t* data, size_t length, uint32_t commit) {
  return little(data + length - 4, 4) == commit && little(data + length - 8, 4) == crc32(data, length - 8);
}

bool enumValue(uint8_t value, uint8_t maximum) { return value >= 1 && value <= maximum; }

bool failurePair(uint8_t flow, uint8_t reason) {
  return (flow == 0 && reason == 0) || (enumValue(flow, 4) && enumValue(reason, 10));
}

bool radioStage(uint8_t value) { return value >= 224 && value <= 246; }

bool runtimeCode(uint8_t value) {
  if (value == 0 || radioStage(value)) return true;
  const unsigned packed = value - 1;
  return packed / 16 < 9 && packed % 16 < 11;
}

bool retainedStageValid(const uint8_t* data, uint8_t code) {
  if (zero(data, 64)) return true;
  return radioStage(code) && data[0] == 1 && (data[1] == 1 || data[1] == 2) && data[2] <= 2 && data[3] == 0 &&
         zero(data + 60, 4);
}

bool operationValid(const uint8_t* data) {
  const uint8_t kind = data[0], first = data[1], second = data[2];
  switch (kind) {
    case 1:
      return enumValue(first, 13) && second == 0;
    case 2:
      return first == 0 && second == 0;
    case 3:
      return enumValue(first, 6) && second == 0;
    case 4:
      return enumValue(first, 9) && second == 0;
    case 5:
      return first == 0 && second == 0;
    case 6:
      return enumValue(first, 12) && second == 0;
    case 7:
      return enumValue(first, 8) && enumValue(second, 8);
    case 8:
      return enumValue(first, 5) && second == 0;
    case 9:
      return first == 0 && second == 0;
    case 10:
      return first == 0 && second == 0;
    case 11:
      return first == 0 && second == 0;
    case 12:
      return first == 0 && second == 0;
    case 13:
      return first == 0 && second == 0;
    default:
      return false;
  }
}

bool outcomeValid(const uint8_t* data) {
  switch (data[0]) {
    case 1:
      return zero(data + 1, 5);
    case 2:
      return enumValue(data[1], 10);
    case 3:
    case 4:
      return zero(data + 1, 5);
    case 5:
      return enumValue(data[1], 5) && zero(data + 2, 4);
    case 6:
      return enumValue(data[1], 4) && zero(data + 2, 4);
    default:
      return false;
  }
}

bool countsValid(const uint8_t* data) {
  switch (data[0]) {
    case 0:
      return zero(data + 1, 24);
    case 1:
      return zero(data + 9, 16);
    case 2:
      return zero(data + 17, 8);
    case 3:
      return true;
    case 4:
    case 5:
      return zero(data + 21, 4);
    default:
      return false;
  }
}

bool perfEventValid(const uint8_t* data, size_t length, uint64_t resetTime) {
  if (length < 2 || data[0] != 2) return false;
  const uint8_t tag = data[1];
  static constexpr size_t lengths[] = {13, 48, 19, 47, 40, 3, 40, 33, 24, 19, 9, 30, 23, 24, 35, 22, 6, 51, 20, 22};
  if (tag < 30 || tag > 49 || length != lengths[tag - 30]) return false;
  auto bounded = [resetTime, data](size_t offset, size_t width) { return little(data + offset, width) <= resetTime; };
  switch (tag) {
    case 30:
      return little(data + 2, 4) != 0 && little(data + 2, 4) != little(data + 6, 4) && operationValid(data + 10);
    case 31:
      return little(data + 2, 4) != 0 && operationValid(data + 6) && outcomeValid(data + 9) && countsValid(data + 23) &&
             bounded(15, 8);
    case 32:
      return enumValue(data[14], 5) && bounded(15, 4);
    case 33:
      return enumValue(data[22], 5) && outcomeValid(data + 23) && bounded(29, 4) && bounded(33, 4) && bounded(37, 4) &&
             bounded(41, 4);
    case 34:
      return operationValid(data + 2) && enumValue(data[5], 6) && enumValue(data[6], 10) && enumValue(data[20], 5) &&
             enumValue(data[39], 3) && bounded(29, 8);
    case 35:
      return true;
    case 36:
      return (data[10] == 0 ? zero(data + 11, 8) : data[10] == 1) && bounded(2, 8) && bounded(11, 8) &&
             bounded(19, 8) && bounded(27, 8);
    case 37:
      return little(data + 2, 8) <= little(data + 18, 8) && little(data + 18, 8) <= little(data + 10, 8) &&
             outcomeValid(data + 27) && bounded(10, 8);
    case 38:
      return little(data + 2, 4) != 0 && enumValue(data[6], 9) && enumValue(data[7], 2) && data[8] <= 1 &&
             data[9] <= 1 && outcomeValid(data + 18) && bounded(10, 8);
    case 39:
      return little(data + 2, 8) != 0 && little(data + 2, 8) <= little(data + 10, 8) && enumValue(data[18], 4);
    case 40:
      return operationValid(data + 6);
    case 41:
      return operationValid(data + 2) && enumValue(data[5], 10) && little(data + 10, 8) <= little(data + 18, 8) &&
             little(data + 26, 4) != 0 && bounded(18, 8);
    case 42:
      return little(data + 3, 8) <= little(data + 11, 8) && little(data + 19, 4) != 0 && bounded(11, 8);
    case 43:
      return enumValue(data[18], 2) && enumValue(data[19], 2);
    case 44:
      return enumValue(data[2], 2);
    case 45:
      return true;
    case 46:
      return true;
    case 47:
      return bounded(6, 8) && bounded(14, 8) && bounded(22, 8);
    case 48:
      return enumValue(data[5], 6) && enumValue(data[6], 2);
    case 49:
      return operationValid(data + 2) && enumValue(data[5], 10) && bounded(10, 8);
    default:
      return true;
  }
}

bool emergencyValid(const uint8_t* data) {
  if (std::memcmp(data, "RMTE", 4) != 0 || data[4] < 1 || data[4] > 3 || little(data + 5, 2) != 512 || data[7] != 0 ||
      !zero(data + 57, 7) || !committed(data, 512, 0x54454c31))
    return false;
  if ((data[36] == 0 && little(data + 32, 4) != 0) || (data[36] == 1 && little(data + 32, 4) == 0) || data[36] > 1 ||
      data[37] > 5 || data[38] > 4 || data[43] > 4 || !runtimeCode(data[55]) || !runtimeCode(data[56]) ||
      !failurePair(data[41], data[42]) || !failurePair(data[49], data[50]))
    return false;
  for (size_t offset : {size_t(39), size_t(48)})
    if (data[offset] > 7) return false;
  if (data[40] > 8 || data[51] > 13 || data[52] > 13 || !retainedStageValid(data + 64, data[55]) ||
      !retainedStageValid(data + 128, data[56]))
    return false;
  const uint16_t rawSteps = little(data + 53, 2);
  const uint16_t steps = rawSteps == 0 && data[51] != 0 ? uint16_t(1u << (data[51] - 1)) : rawSteps;
  if ((steps & ~uint16_t(0x1fff)) != 0 || (steps == 0) != (data[51] == 0) ||
      (steps != 0 && (steps & (1u << (data[51] - 1))) == 0))
    return false;
  if ((data[192] == 0 && !zero(data + 193, 16)) || data[192] > 1 || (data[209] == 0 && little(data + 210, 4) != 0) ||
      data[209] > 1)
    return false;
  if (data[4] == 1) return zero(data + 64, 440);
  if (data[4] == 2) return zero(data + 214, 290);
  if (data[215] != 1 || (data[234] & ~3) != 0 || !zero(data + 235, 5)) return false;
  const size_t lengths[] = {214, 232, 233};
  const size_t payloads[] = {304, 384, 448};
  const size_t capacities[] = {64, 48, 48};
  const size_t sequences[] = {224, 368, 432};
  const size_t times[] = {216, 376, 440};
  for (size_t i = 0; i < 3; ++i) {
    const size_t length = data[lengths[i]];
    if (length > capacities[i] || !zero(data + payloads[i] + length, capacities[i] - length)) return false;
    if (length == 0) {
      if (little(data + sequences[i], 8) != 0 || little(data + times[i], 8) != 0) return false;
    } else if (length < 2 || little(data + sequences[i], 8) == 0 ||
               !perfEventValid(data + payloads[i], length, little(data + times[i], 8))) {
      return false;
    }
  }
  return ((data[234] & 1) != 0 || zero(data + 240, 32)) && ((data[234] & 2) != 0 || zero(data + 272, 32));
}

bool headerValid(const uint8_t* data) {
  return std::memcmp(data, "RMLG", 4) == 0 && little(data + 4, 2) == 2 && (data[6] == 1 || data[6] == 2) &&
         data[7] == 0 && little(data + 8, 8) != 0 && little(data + 16, 4) != 0 && zero(data + 52, 4) &&
         committed(data, 64, 0x324c4752);
}

bool cellValid(const uint8_t* data, uint32_t epoch) {
  const size_t length = data[20];
  return length >= 2 && length <= 64 && little(data, 8) != 0 && little(data + 16, 4) == epoch && zero(data + 21, 3) &&
         zero(data + 24 + length, 64 - length) && committed(data, 96, 0x324c4752);
}
}  // namespace

void RetainedCapture::capture(const volatile uint8_t* source) {
  // Own the bytes before SD setup or a later guest launch can reuse retained storage.
  for (size_t i = 0; i < BYTES; ++i) bytes[i] = source[i];
  count = 0;
  textLength = 0;
  headerIndex = -1;
  savedFormat = Format::Unavailable;
  for (size_t i = 0; i < 2; ++i) validEmergency[i] = emergencyValid(bytes.data() + i * 512);
  const auto* tail = bytes.data() + 1024;
  for (int i = 0; i < 2; ++i) {
    const auto* candidate = tail + i * 64;
    if (headerValid(candidate) && (headerIndex < 0 || little(candidate + 8, 8) >= little(header() + 8, 8))) {
      headerIndex = i;
    }
  }
  if (headerIndex >= 0) {
    savedFormat = Format::Tail;
    const uint32_t epoch = little(header() + 16, 4);
    for (size_t i = 0; i < CELL_COUNT; ++i) {
      const size_t offset = 1024 + 128 + i * 96;
      if (cellValid(bytes.data() + offset, epoch)) cells[count++] = offset;
    }
    std::sort(cells.begin(), cells.begin() + count,
              [this](uint16_t a, uint16_t b) { return little(bytes.data() + a, 8) < little(bytes.data() + b, 8); });
    // A writer never publishes duplicate sequences. Reject both copies if memory disagrees.
    size_t accepted = 0;
    for (size_t i = 0; i < count;) {
      size_t next = i + 1;
      while (next < count && little(bytes.data() + cells[i], 8) == little(bytes.data() + cells[next], 8)) ++next;
      if (next == i + 1) cells[accepted++] = cells[i];
      i = next;
    }
    count = accepted;
    return;
  }
  if (std::memcmp(tail, "RMLG", 4) != 0) return;
  const auto version = little(tail + 4, 2);
  if (version != 1 && version != 2) {
    savedFormat = Format::Unsupported;
    return;
  }
  if (version != 1) return;
  const size_t length = little(tail + 6, 2);
  if (length > 1008 || little(tail + 8, 4) != crc32(tail + 16, length)) return;
  textLength = length;
  savedFormat = Format::LegacyText;
}

const uint8_t* RetainedCapture::emergency(size_t index) const {
  return index < 2 && validEmergency[index] ? bytes.data() + index * 512 : nullptr;
}

const uint8_t* RetainedCapture::newestEmergency() const {
  const auto* a = emergency(0);
  const auto* b = emergency(1);
  if (a == nullptr) return b;
  if (b == nullptr) return a;
  return little(b + 8, 8) > little(a + 8, 8) ? b : a;
}

const uint8_t* RetainedCapture::header() const {
  return headerIndex < 0 ? nullptr : bytes.data() + 1024 + headerIndex * 64;
}

RetainedCapture::Event RetainedCapture::event(size_t index) const {
  if (index >= count) return {};
  const auto* cell = bytes.data() + cells[index];
  return {little(cell, 8), little(cell + 8, 8), cell + 24, cell[20]};
}

const char* RetainedCapture::legacyText() const {
  return savedFormat == Format::LegacyText ? reinterpret_cast<const char*>(bytes.data() + 1040) : nullptr;
}

}  // namespace guest_log
