#include "GuestSlot.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include "FirmwareFlasher.h"

namespace guest_slot {

Inspection inspect() {
  Inspection result;
#if FREEINK_DEVICE_X4PRO
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running || running->type != ESP_PARTITION_TYPE_APP) return result;
  const int slot = static_cast<int>(running->subtype) - static_cast<int>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  if (slot < 0 || slot >= static_cast<int>(ota_boot::kSlotCount)) return result;
  const auto subtype = static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0 + (slot + 1) % 2);
  const esp_partition_t* other = esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, nullptr);
  if (!other) return result;
  auto* buf = static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!buf) {
    LOG_ERR("GUEST", "OOM: slot inspection buffer (4096 bytes)");
    return result;
  }
  const auto read = [](uint32_t offset, void* dst, uint32_t len, void* ctx) {
    return esp_partition_read(static_cast<const esp_partition_t*>(ctx), offset, dst, len) == ESP_OK;
  };
  result.image = ota_boot::inspectGuestImage(read, const_cast<esp_partition_t*>(other), other->size, buf, 4096);
  if (!result.image.installed) {
    heap_caps_free(buf);
    return result;
  }
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
  if (mbedtls_sha256_starts(&sha, 0) == 0 &&
      ota_boot::slotHoldsAppImage(read, const_cast<esp_partition_t*>(other), other->size,
                                  firmware_flash::runningPartitionChipId(), hasher, buf, 4096))
    result.partition = other;
  else
    result.image = {};
  mbedtls_sha256_free(&sha);
  heap_caps_free(buf);
#endif
  return result;
}

}  // namespace guest_slot
