#pragma once

#include <BoardConfig.h>

#include "CpBootRecord.h"

namespace cp_boot {
#if FREEINK_DEVICE_X4PRO
void begin(uint8_t resetReason);
void storageBegin();
void storageEnd(bool ok);
void panelInit();
void firstDraw();
Record snapshot();
#else
inline void begin(uint8_t) {}
inline void storageBegin() {}
inline void storageEnd(bool) {}
inline void panelInit() {}
inline void firstDraw() {}
#endif
}  // namespace cp_boot
