#include "SdGuestUpdate.h"

#if FREEINK_DEVICE_X4PRO
#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "GuestInstall.h"
#include "GuestSlot.h"
#include "OtaBootSwitch.h"
#include "SdGuestUpdateModel.h"
#include "util/TaskWatchdog.h"

namespace sd_guest_update {
namespace {
constexpr char manifestPath[] = "/readmaxer/update.json";
constexpr char imagePath[] = "/readmaxer/update.bin";
constexpr size_t chunkSize = 4096;

struct Workspace {
  uint8_t buffer[chunkSize];
  mbedtls_sha256_context hash;
  Workspace() { mbedtls_sha256_init(&hash); }
  ~Workspace() { mbedtls_sha256_free(&hash); }
};

bool hashImage(Workspace& work, uint32_t size, uint8_t (&digest)[32]) {
  HalFile file;
  if (!Storage.openFileForRead("SDUP", imagePath, file)) return false;
  if (mbedtls_sha256_starts(&work.hash, 0)) return false;
  for (uint32_t offset = 0; offset < size; offset += chunkSize) {
    const uint32_t count = std::min<uint32_t>(chunkSize, size - offset);
    if (file.read(work.buffer, count) != static_cast<int>(count) ||
        mbedtls_sha256_update(&work.hash, work.buffer, count))
      return false;
    resetTaskWatchdogIfSubscribed();
    delay(1);
  }
  return mbedtls_sha256_finish(&work.hash, digest) == 0;
}

bool hashPartition(Workspace& work, const esp_partition_t* partition, uint32_t size, uint8_t (&digest)[32]) {
  if (mbedtls_sha256_starts(&work.hash, 0)) return false;
  for (uint32_t offset = 0; offset < size; offset += chunkSize) {
    const uint32_t count = std::min<uint32_t>(chunkSize, size - offset);
    if (esp_partition_read(partition, offset, work.buffer, count) != ESP_OK ||
        mbedtls_sha256_update(&work.hash, work.buffer, count))
      return false;
    resetTaskWatchdogIfSubscribed();
    delay(1);
  }
  return mbedtls_sha256_finish(&work.hash, digest) == 0;
}

void refuse(Result reason) {
  LOG_ERR("SDUP", "Refused: %s", resultName(reason));
  if (!Storage.remove(manifestPath)) LOG_ERR("SDUP", "Cannot remove update manifest");
}
}  // namespace

bool pending() { return Storage.exists(manifestPath); }

void installPending(void (*showProgress)(uint8_t)) {
  if (!pending()) return;
  if (ota_boot::confirmationPending() || ota_boot::runningSlotCheck() != ota_boot::SlotCheck::Confirmed) {
    LOG_INF("SDUP", "Waiting for OTA confirmation");
    return;
  }
  const auto* dest = esp_ota_get_next_update_partition(nullptr);
  const auto* running = esp_ota_get_running_partition();
  if (!dest || !running || dest->address == running->address) {
    refuse(Result::InstallFailed);
    return;
  }
  auto work = makeUniqueNoThrow<Workspace>();
  if (!work) {
    refuse(Result::NoMemory);
    return;
  }
  HalFile json;
  if (!Storage.openFileForRead("SDUP", manifestPath, json) || json.fileSize64() == 0 || json.fileSize64() > 512 ||
      json.read(work->buffer, json.fileSize()) != static_cast<int>(json.fileSize())) {
    json.close();
    refuse(Result::BadManifest);
    return;
  }
  char mac[13];
  char deviceId[16];
  if (!guest_install::ownMac(mac)) {
    json.close();
    refuse(Result::WrongDevice);
    return;
  }
  std::snprintf(deviceId, sizeof(deviceId), "sr-%s", mac);
  Manifest manifest{};
  const auto checked = parseAndVerify(work->buffer, json.fileSize(), deviceId, dest->size, manifest);
  json.close();
  if (checked != Result::Ok) {
    refuse(checked);
    return;
  }
  HalFile image;
  if (!Storage.openFileForRead("SDUP", imagePath, image)) {
    refuse(Result::MissingImage);
    return;
  }
  if (image.fileSize64() != manifest.size) {
    image.close();
    refuse(Result::WrongSize);
    return;
  }
  image.close();
  uint8_t digest[32];
  if (!hashImage(*work, manifest.size, digest) || std::memcmp(digest, manifest.sha, 32)) {
    refuse(Result::WrongSha);
    return;
  }
  if (hashPartition(*work, dest, manifest.size, digest) && !std::memcmp(digest, manifest.sha, 32)) {
    LOG_INF("SDUP", "Already installed");
    const auto existing = guest_slot::inspectFresh();
    if (existing.partition != dest || !existing.image.installed) {
      refuse(Result::InstallFailed);
      return;
    }
    if (ota_boot::switchTo(dest) != ota_boot::SelectionResult::Selected) {
      refuse(Result::InstallFailed);
      return;
    }
    if (!Storage.remove(manifestPath))
      LOG_ERR("SDUP", "Cannot remove update manifest");
    else if (!Storage.remove(imagePath))
      LOG_ERR("SDUP", "Cannot remove update image");
    ESP.restart();
    return;
  }
  work.reset();
  if (showProgress) showProgress(0);
  char sha[65];
  guest_install::Timing timing;
  const auto installed = guest_install::installFromFiles(manifestPath, imagePath, sha, timing, showProgress);
  if (!guest_install::writeReceipt(sha, installed, timing)) LOG_ERR("SDUP", "Cannot save install receipt");
  if (installed != guest_install::Result::Ok) {
    LOG_ERR("SDUP", "Install: %s", guest_install::resultName(installed));
    refuse(Result::InstallFailed);
    return;
  }
  if (!Storage.remove(manifestPath))
    LOG_ERR("SDUP", "Cannot remove update manifest");
  else if (!Storage.remove(imagePath))
    LOG_ERR("SDUP", "Cannot remove update image");
  LOG_INF("SDUP", "Installed %s", sha);
  ESP.restart();
}
}  // namespace sd_guest_update
#endif
