#pragma once

#include <esp_partition.h>

#include "OtaSlotModel.h"

namespace guest_slot {

struct Inspection {
  ota_boot::GuestImage image;
  const esp_partition_t* partition = nullptr;
};

Inspection inspect();

}  // namespace guest_slot
