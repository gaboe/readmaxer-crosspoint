#include "GuestInstall.h"

#if FREEINK_DEVICE_X4PRO
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <mdns.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "GuestSlot.h"
#include "OtaBootSwitch.h"
#include "util/TaskWatchdog.h"

namespace guest_install {
namespace {
constexpr uint32_t CHUNK = 4096;
constexpr uint32_t ERASE_BLOCK = 64 * 1024;
struct Workspace {
  uint8_t buffer[CHUNK];
  mbedtls_sha256_context imageHash;
  mbedtls_sha256_context fileHash;
  Workspace() {
    mbedtls_sha256_init(&imageHash);
    mbedtls_sha256_init(&fileHash);
  }
  ~Workspace() {
    mbedtls_sha256_free(&imageHash);
    mbedtls_sha256_free(&fileHash);
  }
};

ota_boot::ImageHasher hasher(mbedtls_sha256_context& sha) {
  return {&sha,
          [](void* ctx, const uint8_t* data, uint32_t len) {
            return mbedtls_sha256_update(static_cast<mbedtls_sha256_context*>(ctx), data, len) == 0;
          },
          [](void* ctx, uint8_t (&out)[32]) {
            return mbedtls_sha256_finish(static_cast<mbedtls_sha256_context*>(ctx), out) == 0;
          }};
}

bool readFile(uint32_t offset, void* dst, uint32_t len, void* ctx) {
  auto& file = *static_cast<HalFile*>(ctx);
  resetTaskWatchdogIfSubscribed();
  delay(1);
  return file.seek(offset) && file.read(dst, len) == static_cast<int>(len);
}

Result readManifest(Workspace& work, const char* path, char (&shaHex)[65], uint8_t (&sha)[32]) {
  HalFile file;
  if (!Storage.openFileForRead("GUEST", path, file) || file.fileSize() > 512) return Result::BadManifest;
  const size_t size = file.fileSize();
  if (file.read(work.buffer, size) != static_cast<int>(size)) return Result::ReadFailed;
  JsonDocument doc;
  if (deserializeJson(doc, work.buffer, size) || !doc["sha256"].is<const char*>()) return Result::BadManifest;
  const char* text = doc["sha256"];
  if (!parseSha(text, sha)) return Result::BadManifest;
  std::memcpy(shaHex, text, sizeof(shaHex));
  return Result::Ok;
}

void formatSha(const uint8_t (&sha)[32], char (&shaHex)[65]) {
  for (size_t i = 0; i < 32; ++i) std::snprintf(shaHex + 2 * i, 3, "%02x", sha[i]);
}
}  // namespace

bool ownMac(char (&mac)[13]) {
  uint8_t bytes[6];
  if (esp_read_mac(bytes, ESP_MAC_WIFI_STA) != ESP_OK) return false;
  snprintf(mac, sizeof(mac), "%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
  return true;
}

bool announce() {
  char mac[13];
  char hostname[23];
  if (!ownMac(mac) || !MDNS.begin("crosspoint")) return false;
  snprintf(hostname, sizeof(hostname), "readmaxer-%s", mac);
  mdns_ip_addr_t address{};
  address.addr.type = ESP_IPADDR_TYPE_V4;
  address.addr.u_addr.ip4.addr = static_cast<uint32_t>(WiFi.localIP());
  if (mdns_delegate_hostname_add(hostname, &address) != ESP_OK) {
    MDNS.end();
    return false;
  }
  MDNS.addService("http", "tcp", 80);
  return true;
}

Result install(char (&shaHex)[65], Timing& timing) {
  return installFromFiles("/readmaxer/install.json", "/readmaxer/install.bin", shaHex, timing);
}

Result installFromFiles(const char* manifestPath, const char* imagePath, char (&shaHex)[65], Timing& timing,
                        const VerifiedImage* verified, void (*progress)(uint8_t), SelectionPolicy selectionPolicy) {
  timing = {};
  shaHex[0] = '\0';
  const auto* running = esp_ota_get_running_partition();
  const auto* dest = esp_ota_get_next_update_partition(nullptr);
  if (!running || !dest || dest->address == running->address || dest->size % CHUNK != 0) return Result::NoPartition;
  // Refuse without changing otadata. A pending fallback must reach Home first.
  if (ota_boot::confirmationPending() || ota_boot::runningSlotCheck() != ota_boot::SlotCheck::Confirmed)
    return Result::GuardRefused;
  auto work = makeUniqueNoThrow<Workspace>();
  if (!work) {
    LOG_ERR("GUEST", "OOM: install workspace (%u bytes)", static_cast<unsigned>(sizeof(Workspace)));
    return Result::NoMemory;
  }
  uint8_t expected[32];
  const uint32_t manifestStart = millis();
  Result manifest = Result::Ok;
  if (verified) {
    std::memcpy(expected, verified->sha, sizeof(expected));
    formatSha(expected, shaHex);
  } else {
    manifest = readManifest(*work, manifestPath, shaHex, expected);
  }
  timing.manifest = millis() - manifestStart;
  if (manifest != Result::Ok) return manifest;
  char mac[13];
  if (!ownMac(mac)) return Result::BadIdentity;
  HalFile file;
  if (!Storage.openFileForRead("GUEST", imagePath, file)) return Result::ReadFailed;
  const auto size64 = file.fileSize64();
  if (size64 > dest->size) return Result::TooLarge;
  if (verified && size64 != verified->size) return Result::WrongSize;
  const uint32_t size = static_cast<uint32_t>(size64);
  timing.imageBytes = size;
  if (mbedtls_sha256_starts(&work->imageHash, 0) || mbedtls_sha256_starts(&work->fileHash, 0))
    return Result::ReadFailed;
  const uint32_t validateStart = millis();
  const auto checked = validate(readFile, &file, size, dest->size, mac, expected, hasher(work->imageHash),
                                hasher(work->fileHash), work->buffer, CHUNK);
  timing.validate = millis() - validateStart;
  if (checked != Result::Ok) return checked;

  {
    ota_boot::AppWriteGuard guard(dest);
    if (!guard) return Result::GuardRefused;
    guest_slot::invalidateCache();
    const uint32_t writeStart = millis();
    uint8_t shownQuarter = 0;
    for (uint32_t pos = 0; pos < size; pos += CHUNK) {
      const uint32_t len = std::min(CHUNK, size - pos);
      if (!readFile(pos, work->buffer, len, &file)) {
        timing.eraseWrite = millis() - writeStart;
        return Result::ReadFailed;
      }
      if (pos % ERASE_BLOCK == 0 &&
          esp_partition_erase_range(dest, pos, std::min(ERASE_BLOCK, dest->size - pos)) != ESP_OK) {
        timing.eraseWrite = millis() - writeStart;
        return Result::WriteFailed;
      }
      if (esp_partition_write(dest, pos, work->buffer, len) != ESP_OK) {
        timing.eraseWrite = millis() - writeStart;
        return Result::WriteFailed;
      }
      const uint8_t quarter = static_cast<uint8_t>((static_cast<uint64_t>(pos + len) * 4) / size);
      if (progress && quarter > shownQuarter) {
        shownQuarter = quarter;
        progress(static_cast<uint8_t>(quarter * 25));
      }
    }
    timing.eraseWrite = millis() - writeStart;
    const uint32_t readbackStart = millis();
    if (mbedtls_sha256_starts(&work->fileHash, 0)) return Result::ReadFailed;
    for (uint32_t pos = 0; pos < size; pos += CHUNK) {
      const uint32_t len = std::min(CHUNK, size - pos);
      if (esp_partition_read(dest, pos, work->buffer, len) != ESP_OK ||
          mbedtls_sha256_update(&work->fileHash, work->buffer, len)) {
        timing.readbackHash = millis() - readbackStart;
        return Result::ReadFailed;
      }
      resetTaskWatchdogIfSubscribed();
      delay(1);
    }
    uint8_t actual[32];
    if (mbedtls_sha256_finish(&work->fileHash, actual)) {
      timing.readbackHash = millis() - readbackStart;
      return Result::ReadFailed;
    }
    timing.readbackHash = millis() - readbackStart;
    if (std::memcmp(actual, expected, sizeof(actual))) return Result::BadReadback;
  }
  if (selectionPolicy == SelectionPolicy::DeferUntilCleanup) return Result::Ok;
  const uint32_t selectionStart = millis();
  const auto selection = ota_boot::switchTo(dest);
  timing.selection = millis() - selectionStart;
  switch (selection) {
    case ota_boot::SelectionResult::Selected:
      return Result::Ok;
    case ota_boot::SelectionResult::Failed:
      return Result::SelectionFailed;
    case ota_boot::SelectionResult::Unknown:
      return Result::SelectionUnknown;
  }
  return Result::SelectionFailed;
}

bool formatTiming(char* out, size_t capacity, const Timing& timing) {
  const int len =
      snprintf(out, capacity,
               "\"timing_ms\":{\"upload_receive\":%lu,\"manifest\":%lu,\"validate\":%lu,"
               "\"erase_write\":%lu,\"readback_hash\":%lu,\"selection\":%lu},"
               "\"bytes\":{\"upload\":%lu,\"image\":%lu}",
               static_cast<unsigned long>(timing.uploadReceive), static_cast<unsigned long>(timing.manifest),
               static_cast<unsigned long>(timing.validate), static_cast<unsigned long>(timing.eraseWrite),
               static_cast<unsigned long>(timing.readbackHash), static_cast<unsigned long>(timing.selection),
               static_cast<unsigned long>(timing.uploadBytes), static_cast<unsigned long>(timing.imageBytes));
  return len >= 0 && static_cast<size_t>(len) < capacity;
}

bool writeReceipt(const char* shaHex, Result result, const Timing& timing) {
  char text[512];
  char details[256];
  if (!formatTiming(details, sizeof(details), timing)) return false;
  const int len = snprintf(text, sizeof(text), "{\"sha256\":\"%s\",\"result\":\"%s\",\"time\":%lld,%s}\n", shaHex,
                           resultName(result), static_cast<long long>(time(nullptr)), details);
  HalFile file;
  if (len < 0 || static_cast<size_t>(len) >= sizeof(text) ||
      !Storage.openFileForWrite("GUEST", "/readmaxer/last-install.tmp", file))
    return false;
  const bool written = file.write(reinterpret_cast<const uint8_t*>(text), len) == static_cast<size_t>(len);
  const bool closed = file.close();
  if (!written || !closed) return false;
  constexpr auto receipt = "/readmaxer/last-install.json";
  constexpr auto backup = "/readmaxer/last-install.bak";
  const bool hadReceipt = Storage.exists(receipt);
  if (hadReceipt && ((Storage.exists(backup) && !Storage.remove(backup)) || !Storage.rename(receipt, backup)))
    return false;
  if (!Storage.rename("/readmaxer/last-install.tmp", receipt)) {
    if (hadReceipt && !Storage.rename(backup, receipt)) LOG_ERR("GUEST", "Previous receipt remains in .bak");
    return false;
  }
  if (Storage.exists(backup)) Storage.remove(backup);
  return true;
}
}  // namespace guest_install
#endif
