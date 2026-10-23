#pragma once

#include <BoardConfig.h>

#include "GuestInstallModel.h"

#if FREEINK_DEVICE_X4PRO
namespace guest_install {

// Fixed SD paths are served only while GuestInstallActivity owns the web server.
Result install(char (&shaHex)[65]);
bool writeReceipt(const char* shaHex, Result result);
bool ownMac(char (&mac)[13]);
bool announce();

}  // namespace guest_install
#endif
