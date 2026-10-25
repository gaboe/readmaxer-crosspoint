#pragma once

#include <BoardConfig.h>

#include <cstddef>
#include <cstdint>

#include "GuestInstallModel.h"

#if FREEINK_DEVICE_X4PRO
namespace guest_install {

struct Timing {
  uint32_t uploadReceive = 0;
  uint32_t manifest = 0;
  uint32_t validate = 0;
  uint32_t eraseWrite = 0;
  uint32_t readbackHash = 0;
  uint32_t selection = 0;
  uint32_t uploadBytes = 0;
  uint32_t imageBytes = 0;
};

// Fixed SD paths are served only while GuestInstallActivity owns the web server.
Result install(char (&shaHex)[65], Timing& timing);
Result installFromFiles(const char* manifestPath, const char* imagePath, char (&shaHex)[65], Timing& timing,
                        void (*progress)(uint8_t) = nullptr);
bool writeReceipt(const char* shaHex, Result result, const Timing& timing);
bool formatTiming(char* out, size_t capacity, const Timing& timing);
bool ownMac(char (&mac)[13]);
bool announce();

}  // namespace guest_install
#endif
