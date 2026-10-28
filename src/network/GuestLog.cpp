#include "GuestLog.h"

#if FREEINK_DEVICE_X4PRO
#include <cstdint>

namespace guest_log {
namespace {
constexpr size_t HEADER_SIZE = 16;
constexpr size_t MAX_TEXT = 1024 - HEADER_SIZE;
RetainedCapture retained;
char saved[MAX_TEXT + 1];
size_t savedLength = 0;
bool savedValid = false;

}  // namespace

void captureAtBoot() {
  savedValid = false;
  savedLength = 0;
  retained.capture(reinterpret_cast<const volatile uint8_t*>(RetainedCapture::ADDRESS));
  if (retained.format() != RetainedCapture::Format::LegacyText) return;
  savedLength = retained.legacyLength();
  for (size_t i = 0; i < savedLength; ++i) saved[i] = retained.legacyText()[i];
  saved[savedLength] = '\0';
  savedValid = true;
}

bool available() { return savedValid; }
const char* text() { return saved; }
size_t length() { return savedLength; }
const RetainedCapture& retainedCapture() { return retained; }

}  // namespace guest_log
#endif
