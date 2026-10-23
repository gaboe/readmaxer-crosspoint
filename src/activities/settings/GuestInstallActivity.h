#pragma once

#include <BoardConfig.h>

#if FREEINK_DEVICE_X4PRO
#include <I18n.h>

#include <memory>

#include "activities/Activity.h"
#include "network/CrossPointWebServer.h"

class GuestInstallActivity final : public Activity {
 public:
  GuestInstallActivity(GfxRenderer& renderer, MappedInputManager& input) : Activity("GuestInstall", renderer, input) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state != State::Failed; }

 private:
  enum class State { Connecting, Waiting, Installing, Failed };
  State state = State::Connecting;
  StrId message = StrId::STR_GUEST_CONNECTING;
  const char* resultCode = "";
  char hostname[29]{};
  char ip[16]{};
  unsigned long connectionStarted = 0;
  bool leaveRequested = false;
  std::unique_ptr<CrossPointWebServer> server;

  void fail(StrId message);
  void stopNetwork();
  static void installStatus(bool installing, const char* result, void* ctx);
};
#endif
