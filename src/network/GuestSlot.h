#pragma once

#include <esp_partition.h>

#include "GuestSlotModel.h"

namespace guest_slot {

struct Inspection {
  GuestImage image;
  bool bootableNonGuest = false;
  const esp_partition_t* partition = nullptr;
};

enum class SwitchResult { NoGuest, GuardRefused, SelectionFailed, Selected, Unknown };

Inspection inspect();
Inspection inspectFresh();
bool switchAllowed();
SwitchResult switchGuest();
bool bootableNonGuest(const esp_partition_t* partition);
bool checkAndCache(const esp_partition_t* other);
void invalidateCache();

}  // namespace guest_slot
