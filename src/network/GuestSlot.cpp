#include "GuestSlot.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include "FirmwareFlasher.h"
#include "OtaBootSwitch.h"

namespace guest_slot {

namespace {
Inspection cached;
bool cacheReady = false;

const esp_partition_t* otherPartition() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running || running->type != ESP_PARTITION_TYPE_APP) return nullptr;
  const int slot = static_cast<int>(running->subtype) - static_cast<int>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  if (slot < 0 || slot >= static_cast<int>(ota_boot::kSlotCount)) return nullptr;
  const auto subtype = static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0 + (slot + 1) % 2);
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, nullptr);
}

bool inspectPartition(const esp_partition_t* other, Inspection& result) {
  if (!other) return false;
  auto* buf = static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!buf) {
    LOG_ERR("GUEST", "OOM: slot inspection buffer (4096 bytes)");
    return false;
  }
  const auto read = [](uint32_t offset, void* dst, uint32_t len, void* ctx) {
    return esp_partition_read(static_cast<const esp_partition_t*>(ctx), offset, dst, len) == ESP_OK;
  };
  auto candidate = inspectHeader(read, const_cast<esp_partition_t*>(other), other->size);
  Scanner scanner;
  const auto visit = [](const uint8_t* bytes, uint32_t len, void* ctx) {
    static_cast<Scanner*>(ctx)->feed(bytes, len);
  };
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  const ota_boot::ImageHasher hasher{
      &sha,
      [](void* ctx, const uint8_t* data, uint32_t len) {
        return mbedtls_sha256_update(static_cast<mbedtls_sha256_context*>(ctx), data, len) == 0;
      },
      [](void* ctx, uint8_t (&digest)[32]) {
        return mbedtls_sha256_finish(static_cast<mbedtls_sha256_context*>(ctx), digest) == 0;
      }};
  const bool valid =
      mbedtls_sha256_starts(&sha, 0) == 0 &&
      ota_boot::slotHoldsAppImage(read, const_cast<esp_partition_t*>(other), other->size,
                                  firmware_flash::runningPartitionChipId(), hasher, buf, 4096, visit, &scanner);
  mbedtls_sha256_free(&sha);
  heap_caps_free(buf);
  if (valid && candidate.installed && scanner.exactlyOne()) {
    result.image = candidate;
    result.partition = other;
  }
  return valid;
}
}  // namespace

Inspection inspectFresh() {
  Inspection result;
#if FREEINK_DEVICE_X4PRO
  inspectPartition(otherPartition(), result);
#endif
  return result;
}

Inspection inspect() {
#if FREEINK_DEVICE_X4PRO
  if (!cacheReady) {
    cached = inspectFresh();
    cacheReady = true;
  }
#endif
  return cached;
}

bool switchAllowed() {
  return canSwitch(ota_boot::runningSlotConfirmed() ? ota_boot::SlotCheck::Confirmed : ota_boot::SlotCheck::Unconfirmed,
                   ota_boot::confirmationPending());
}

bool switchGuest() {
  if (!switchAllowed() || !canSwitch(ota_boot::runningSlotCheck(), ota_boot::confirmationPending())) return false;
  const Inspection fresh = inspectFresh();
  return fresh.image.installed && fresh.partition && switchAllowed() &&
         canSwitch(ota_boot::runningSlotCheck(), ota_boot::confirmationPending()) &&
         ota_boot::switchTo(fresh.partition, ota_boot::kOtaImgNew);
}

bool checkAndCache(const esp_partition_t* other) {
#if FREEINK_DEVICE_X4PRO
  cached = {};
  cacheReady = true;
  return inspectPartition(other, cached);
#else
  (void)other;
  return false;
#endif
}

void invalidateCache() {
  cached = {};
  cacheReady = false;
}

}  // namespace guest_slot
