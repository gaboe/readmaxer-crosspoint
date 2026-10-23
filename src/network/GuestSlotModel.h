#pragma once

#include <cstdint>

#include "OtaSlotModel.h"

namespace guest_slot {

struct GuestImage {
  bool installed = false;
  char version[33]{};
};

class Scanner {
 public:
  void feed(const uint8_t* data, uint32_t len);
  uint8_t count() const { return occurrences; }
  bool exactlyOne() const { return occurrences == 1; }

 private:
  uint8_t matched = 0;
  uint8_t occurrences = 0;
};

GuestImage inspectHeader(ota_boot::SlotReader read, void* ctx, uint32_t slotSize);
bool canSwitch(ota_boot::SlotCheck check, bool confirmationPending);

}  // namespace guest_slot
