#pragma once

namespace guest_devloop {

constexpr char kFlagPath[] = "/readmaxer/devloop";

constexpr bool shouldOpen(bool flagExists, bool recovery, bool panic, bool confirmationPending) {
  return flagExists && !recovery && !panic && !confirmationPending;
}

}  // namespace guest_devloop
