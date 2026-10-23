#pragma once

#include <esp_partition.h>

#include "GuestSlotModel.h"

namespace guest_slot {

struct Inspection {
  GuestImage image;
  const esp_partition_t* partition = nullptr;
};

Inspection inspect();
Inspection inspectFresh();
bool switchAllowed();
bool switchGuest();
bool checkAndCache(const esp_partition_t* other);
void invalidateCache();

}  // namespace guest_slot
