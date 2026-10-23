#include "GuestLog.h"

#if FREEINK_DEVICE_X4PRO
#include <cstdint>

namespace guest_log {
namespace {
constexpr uintptr_t RTC_ADDRESS = 0x50001c00;
constexpr size_t HEADER_SIZE = 16;
constexpr size_t MAX_TEXT = 1024 - HEADER_SIZE;
constexpr uint32_t MAGIC = 0x474c4d52;

struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t length;
  uint32_t crc32;
  uint32_t bootSeq;
};
static_assert(sizeof(Header) == HEADER_SIZE);

char saved[MAX_TEXT + 1];
size_t savedLength = 0;
bool savedValid = false;

uint32_t crc32(const char* data, size_t length) {
  uint32_t crc = 0xffffffff;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint8_t>(data[i]);
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return ~crc;
}
}  // namespace

void captureAtBoot() {
  savedValid = false;
  savedLength = 0;
  const auto* rtc = reinterpret_cast<const volatile uint8_t*>(RTC_ADDRESS);
  Header header{};
  auto* headerBytes = reinterpret_cast<uint8_t*>(&header);
  for (size_t i = 0; i < HEADER_SIZE; ++i) headerBytes[i] = rtc[i];
  if (header.magic != MAGIC || header.version != 1 || header.length > MAX_TEXT) return;
  for (size_t i = 0; i < header.length; ++i) saved[i] = static_cast<char>(rtc[HEADER_SIZE + i]);
  if (crc32(saved, header.length) != header.crc32) return;
  savedLength = header.length;
  saved[savedLength] = '\0';
  savedValid = true;
}

bool available() { return savedValid; }
const char* text() { return saved; }
size_t length() { return savedLength; }

}  // namespace guest_log
#endif
