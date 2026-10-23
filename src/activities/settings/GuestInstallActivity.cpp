#include "GuestInstallActivity.h"

#if FREEINK_DEVICE_X4PRO
#include <ESPmDNS.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/GuestInstall.h"

void GuestInstallActivity::onEnter() {
  Activity::onEnter();
  if (auto* fonts = renderer.getFontCacheManager()) fonts->releaseSdFontCaches();
  WIFI_STORE.loadFromFile();
  auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!credential) credential = WIFI_STORE.getCredentialAt(0);
  if (!credential) {
    fail(StrId::STR_GUEST_NO_WIFI);
    return;
  }
  char mac[13];
  if (!guest_install::ownMac(mac) || !Storage.ensureDirectoryExists("/readmaxer")) {
    fail(StrId::STR_GUEST_START_FAILED);
    return;
  }
  {
    RenderLock lock;
    snprintf(hostname, sizeof(hostname), "readmaxer-%s.local", mac);
  }
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  connectionStarted = millis();
  requestUpdate();
}

void GuestInstallActivity::stopNetwork() {
  if (server) server->stop();
  server.reset();
  MDNS.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void GuestInstallActivity::onExit() {
  stopNetwork();
  Activity::onExit();
}

void GuestInstallActivity::fail(StrId error) {
  LOG_ERR("GUEST", "Install activity failed (%u)", static_cast<unsigned>(error));
  stopNetwork();
  {
    RenderLock lock;
    state = State::Failed;
    message = error;
  }
  requestUpdate();
}

void GuestInstallActivity::installStatus(bool installing, const char* result, void* ctx) {
  auto& activity = *static_cast<GuestInstallActivity*>(ctx);
  {
    RenderLock lock;
    activity.state = installing ? State::Installing : State::Waiting;
    activity.message = installing ? StrId::STR_GUEST_INSTALLING : StrId::STR_GUEST_RESULT;
    activity.resultCode = result;
  }
  activity.requestUpdateAndWait();
}

void GuestInstallActivity::loop() {
  if (leaveRequested || mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  switch (state) {
    case State::Connecting:
      if (WiFi.status() == WL_CONNECTED) {
        if (!guest_install::announce()) {
          fail(StrId::STR_GUEST_START_FAILED);
          return;
        }
        server = makeUniqueNoThrow<CrossPointWebServer>();
        if (!server) {
          LOG_ERR("GUEST", "OOM: web server (%u bytes)", static_cast<unsigned>(sizeof(CrossPointWebServer)));
          fail(StrId::STR_MEMORY_ERROR);
          return;
        }
        server->setRemoteInput(mappedInput);
        server->enableGuestInstall(installStatus, this);
        server->setUploadCancelCheck([this] {
          mappedInput.update(true);
          if (gpio.rawInputActive()) {
            delay(6);
            mappedInput.update(true);
          }
          leaveRequested = leaveRequested || mappedInput.isPressed(MappedInputManager::Button::Back) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture();
          return leaveRequested;
        });
        server->begin();
        if (!server->isRunning()) {
          fail(StrId::STR_GUEST_START_FAILED);
          return;
        }
        {
          RenderLock lock;
          const auto address = WiFi.localIP();
          snprintf(ip, sizeof(ip), "%u.%u.%u.%u", address[0], address[1], address[2], address[3]);
          state = State::Waiting;
          message = StrId::STR_GUEST_WAITING;
        }
        requestUpdate();
      } else if (millis() - connectionStarted >= 30000) {
        fail(StrId::STR_GUEST_WIFI_FAILED);
      }
      break;
    case State::Waiting:
      if (WiFi.status() != WL_CONNECTED) {
        fail(StrId::STR_GUEST_WIFI_FAILED);
        return;
      }
      server->handleClient();
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) finish();
      break;
    case State::Installing:
    case State::Failed:
      break;
  }
}

void GuestInstallActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_INSTALL_READMAXER));
  const int middle = renderer.getScreenHeight() / 2;
  renderer.drawCenteredText(UI_12_FONT_ID, middle - 40, I18N.get(message));
  if (state == State::Waiting) {
    renderer.drawCenteredText(UI_10_FONT_ID, middle, hostname);
    renderer.drawCenteredText(UI_10_FONT_ID, middle + 25, ip);
    renderer.drawCenteredText(UI_10_FONT_ID, middle + 50, resultCode);
  }
  if (state != State::Installing) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
#endif
