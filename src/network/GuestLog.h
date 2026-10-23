#pragma once

#include <BoardConfig.h>

#include <cstddef>

#if FREEINK_DEVICE_X4PRO
namespace guest_log {

void captureAtBoot();
bool available();
const char* text();
size_t length();

}  // namespace guest_log
#endif
