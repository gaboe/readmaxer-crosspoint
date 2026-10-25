#pragma once

#include <BoardConfig.h>

#include <cstdint>

#if FREEINK_DEVICE_X4PRO
namespace sd_guest_update {
bool pending();
void installPending(void (*showProgress)(uint8_t));
}  // namespace sd_guest_update
#endif
