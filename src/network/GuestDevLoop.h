#pragma once

namespace guest_devloop {

constexpr char kFlagPath[] = "/readmaxer/devloop";

enum class BootRoute {
  Normal,
  ConfirmInHome,
  Install,
};

constexpr BootRoute route(bool flagExists, bool recovery, bool panic, bool confirmationPending) {
  if (!flagExists || recovery || panic) return BootRoute::Normal;
  return confirmationPending ? BootRoute::ConfirmInHome : BootRoute::Install;
}

}  // namespace guest_devloop
