#pragma once

#include <BoardConfig.h>

#include <cstddef>

#include "RetainedCapture.h"

#if FREEINK_DEVICE_X4PRO
namespace guest_log {

void captureAtBoot();
bool available();
const char* text();
size_t length();
const RetainedCapture& retainedCapture();

}  // namespace guest_log
#endif
