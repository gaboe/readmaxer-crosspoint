#include "OtaBootSwitch.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <spi_flash_mmap.h>

#include <atomic>

#include "FirmwareFlasher.h"

#if FREEINK_DEVICE_X4PRO
// initArduino() marks a PENDING_VERIFY image valid before setup() unless this
// returns true. confirmRunningSlot() does it once Home has rendered. Other
// boards keep the weak default.
extern "C" bool verifyRollbackLater() { return true; }
#endif

namespace ota_boot {

namespace {

std::atomic<bool> pendingConfirmation{false};
bool selectedOtherSlot = false;
int64_t imageCheckUs = -1;

int otaSlotOf(const esp_partition_t* partition) {
  if (!partition || partition->type != ESP_PARTITION_TYPE_APP) return -1;
  const int slot = static_cast<int>(partition->subtype) - static_cast<int>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  return slot >= 0 && slot < static_cast<int>(kSlotCount) ? slot : -1;
}

const esp_partition_t* findOtadata() {
  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata || otadata->size < kEntrySectors * SPI_FLASH_SEC_SIZE) return nullptr;
  return otadata;
}

bool readEntries(const esp_partition_t* otadata, SelectEntry (&entries)[kEntrySectors]) {
  for (uint32_t i = 0; i < kEntrySectors; ++i) {
    if (esp_partition_read(otadata, i * SPI_FLASH_SEC_SIZE, &entries[i], sizeof(SelectEntry)) != ESP_OK) return false;
  }
  return true;
}

bool writeEntry(const esp_partition_t* otadata, const EntryWrite& write) {
  const size_t offset = static_cast<size_t>(write.sector) * SPI_FLASH_SEC_SIZE;
  return esp_partition_erase_range(otadata, offset, SPI_FLASH_SEC_SIZE) == ESP_OK &&
         esp_partition_write(otadata, offset, &write.entry, sizeof(write.entry)) == ESP_OK;
}

#if FREEINK_DEVICE_X4PRO
constexpr uint32_t kImageCheckChunk = 4096;

bool readSlot(uint32_t offset, void* dst, uint32_t len, void* ctx) {
  return esp_partition_read(static_cast<const esp_partition_t*>(ctx), offset, dst, len) == ESP_OK;
}

bool shaUpdate(void* ctx, const uint8_t* data, uint32_t len) {
  return mbedtls_sha256_update(static_cast<mbedtls_sha256_context*>(ctx), data, len) == 0;
}

bool shaFinish(void* ctx, uint8_t (&digest)[32]) {
  return mbedtls_sha256_finish(static_cast<mbedtls_sha256_context*>(ctx), digest) == 0;
}

bool otherSlotHoldsImage(const esp_partition_t* other) {
  const uint16_t chipId = firmware_flash::runningPartitionChipId();
  if (!other || chipId == 0xFFFF) return false;
  // Internal RAM, freed after the check: SHA hardware may read it by DMA.
  auto* const buf = static_cast<uint8_t*>(heap_caps_malloc(kImageCheckChunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!buf) return false;
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  const ImageHasher hasher{&sha, shaUpdate, shaFinish};
  const bool holds = mbedtls_sha256_starts(&sha, /*is224=*/0) == 0 &&
                     slotHoldsAppImage(readSlot, const_cast<esp_partition_t*>(other), other->size, chipId, hasher, buf,
                                       kImageCheckChunk);
  mbedtls_sha256_free(&sha);
  heap_caps_free(buf);
  return holds;
}
#endif

}  // namespace

bool switchTo(const esp_partition_t* dest, const uint32_t state) {
  const int slot = otaSlotOf(dest);
  if (slot < 0) {
    LOG_ERR("BOOT", "dest is not an OTA app slot");
    return false;
  }
  const esp_partition_t* otadata = findOtadata();
  SelectEntry entries[kEntrySectors];
  if (!otadata || !readEntries(otadata, entries)) {
    LOG_ERR("BOOT", "otadata missing or unreadable");
    return false;
  }
  const EntryWrite write = planSelect(entries, static_cast<uint32_t>(slot), state);
  if (!writeEntry(otadata, write)) {
    LOG_ERR("BOOT", "otadata write failed (sector=%u)", static_cast<unsigned>(write.sector));
    return false;
  }
  LOG_INF("BOOT", "otadata: wrote sector=%u seq=%u state=%u -> %s", static_cast<unsigned>(write.sector),
          static_cast<unsigned>(write.entry.ota_seq), static_cast<unsigned>(state), dest->label);
  return true;
}

void armBootFallback() {
#if FREEINK_DEVICE_X4PRO
  const int running = otaSlotOf(esp_ota_get_running_partition());
  const esp_partition_t* otadata = findOtadata();
  SelectEntry entries[kEntrySectors];
  if (running < 0 || !otadata || !readEntries(otadata, entries)) return;

  const auto runningSlot = static_cast<uint32_t>(running);
  const BootPlan plan = planBoot(entries, runningSlot, esp_reset_reason() == ESP_RST_DEEPSLEEP);
  pendingConfirmation = plan.confirmAfterHome;
  if (!plan.armFallback) return;

  const auto otherSlot = (runningSlot + 1) % kSlotCount;
  const esp_partition_t* other = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0 + otherSlot),
      nullptr);
  const int64_t started = esp_timer_get_time();
  const bool holds = otherSlotHoldsImage(other);
  imageCheckUs = esp_timer_get_time() - started;
  if (holds) selectedOtherSlot = writeEntry(otadata, planSelect(entries, otherSlot, kOtaImgValid));
#endif
}

bool keepRunningSlot() {
  if (!pendingConfirmation) return true;
  const int running = otaSlotOf(esp_ota_get_running_partition());
  const esp_partition_t* otadata = findOtadata();
  SelectEntry entries[kEntrySectors];
  if (running < 0 || !otadata || !readEntries(otadata, entries)) {
    LOG_ERR("BOOT", "keep running slot: otadata missing or unreadable");
    return false;
  }
  EntryWrite write{};
  if (!planKeepRunningSlot(entries, static_cast<uint32_t>(running), write)) return true;
  if (!writeEntry(otadata, write)) {
    LOG_ERR("BOOT", "keep running slot: otadata write failed (sector=%u)", static_cast<unsigned>(write.sector));
    return false;
  }
  selectedOtherSlot = false;
  LOG_INF("BOOT", "running slot kept for the next boot (sector=%u seq=%u state=NEW)",
          static_cast<unsigned>(write.sector), static_cast<unsigned>(write.entry.ota_seq));
  return true;
}

bool confirmRunningSlot() {
  if (!pendingConfirmation) return true;
  // Select the running slot first: esp_ota_mark_app_valid_cancel_rollback()
  // marks whichever entry is active, which is the other slot while armed.
  if (!switchTo(esp_ota_get_running_partition(), kOtaImgValid)) {
    LOG_ERR("BOOT", "confirm running slot failed");
    return false;
  }
  pendingConfirmation = false;
  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  LOG_INF("BOOT", "running slot confirmed (fallback was %s, other-slot image check %lld us, cancel rollback: %s)",
          selectedOtherSlot ? "armed" : "not armed", static_cast<long long>(imageCheckUs), esp_err_to_name(err));
  return true;
}

bool confirmationPending() { return pendingConfirmation; }

}  // namespace ota_boot
