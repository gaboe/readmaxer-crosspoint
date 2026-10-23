#include "GuestSlotModel.h"

#include <array>
#include <cstring>

namespace guest_slot {

namespace {
constexpr char kPrefix[] = "READMAXER-";
constexpr char kSuffix[] = "GUEST-V1;";
constexpr uint8_t kPrefixLen = sizeof(kPrefix) - 1;
constexpr uint8_t kMarkerLen = kPrefixLen + sizeof(kSuffix) - 1;

constexpr char needleAt(uint8_t pos) { return pos < kPrefixLen ? kPrefix[pos] : kSuffix[pos - kPrefixLen]; }

constexpr std::array<uint8_t, kMarkerLen> failureTable() {
  std::array<uint8_t, kMarkerLen> failure{};
  uint8_t matched = 0;
  for (uint8_t pos = 1; pos < kMarkerLen; ++pos) {
    while (matched && needleAt(pos) != needleAt(matched)) matched = failure[matched - 1];
    if (needleAt(pos) == needleAt(matched)) ++matched;
    failure[pos] = matched;
  }
  return failure;
}

constexpr auto kFailure = failureTable();
}  // namespace

void Scanner::feed(const uint8_t* data, uint32_t len) {
  for (uint32_t i = 0; i < len; ++i) {
    const char c = static_cast<char>(data[i]);
    while (matched && c != needleAt(matched)) matched = kFailure[matched - 1];
    if (c != needleAt(matched)) continue;
    if (++matched == kMarkerLen) {
      if (occurrences < 2) ++occurrences;
      matched = kFailure[matched - 1];
    }
  }
}

GuestImage inspectHeader(ota_boot::SlotReader read, void* ctx, uint32_t slotSize) {
  GuestImage result;
  constexpr char project[] = "readmaxer-xteink-x4pro";
  if (slotSize < 112) return result;
  uint8_t header[32];
  if (!read(0, header, sizeof(header), ctx) || header[0] != 0xE9 || header[1] == 0 || header[1] > 16)
    return result;
  uint32_t firstLength;
  std::memcpy(&firstLength, header + 28, sizeof(firstLength));
  if (firstLength < 80 || firstLength > slotSize - 32) return result;
  uint8_t descriptor[80];
  if (!read(32, descriptor, sizeof(descriptor), ctx)) return result;
  uint32_t magic;
  std::memcpy(&magic, descriptor, sizeof(magic));
  if (magic != 0xABCD5432 || std::memcmp(descriptor + 48, project, sizeof(project)) != 0 ||
      std::memchr(descriptor + 16, 0, 32) == nullptr)
    return result;
  std::memcpy(result.version, descriptor + 16, 32);
  result.installed = true;
  return result;
}

bool canSwitch(ota_boot::SlotCheck check, bool confirmationPending) {
  return !confirmationPending && check == ota_boot::SlotCheck::Confirmed;
}

}  // namespace guest_slot
