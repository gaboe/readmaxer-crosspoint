#include "OtaBootSwitch.h"

#include <Arduino.h>
#include <Logging.h>
#include <esp_ota_ops.h>
#include <esp_sleep.h>
#include <mbedtls/sha256.h>
#include <spi_flash_mmap.h>
#include <string.h>

namespace ota_boot {

namespace {
constexpr size_t kOtaSectorSize = SPI_FLASH_SEC_SIZE;
#if FREEINK_DEVICE_X4PRO
uint8_t appValidationBuffer[4096];
#endif

const esp_partition_t* findOtadata() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
}

bool readModel(const esp_partition_t* otadata, uint8_t model[kModelSize]) {
  if (!otadata) {
    LOG_ERR("BOOT", "otadata partition not found");
    return false;
  }
  if (otadata->size < 2 * kOtaSectorSize) {
    LOG_ERR("BOOT", "otadata too small: %u", static_cast<unsigned>(otadata->size));
    return false;
  }
  if (esp_partition_read(otadata, 0, model, kSelectEntrySize) != ESP_OK ||
      esp_partition_read(otadata, kOtaSectorSize, model + kSelectEntrySize, kSelectEntrySize) != ESP_OK) {
    LOG_ERR("BOOT", "otadata read failed");
    return false;
  }
  return true;
}

#if FREEINK_DEVICE_X4PRO
bool hasValidImageHeader(const esp_partition_t* partition, uint16_t chipId) {
  if (!partition || partition->size < 24) return false;
  uint8_t header[24];
  if (esp_partition_read(partition, 0, header, sizeof(header)) != ESP_OK || header[0] != 0xE9 || header[1] == 0 ||
      header[12] != static_cast<uint8_t>(chipId) || header[13] != static_cast<uint8_t>(chipId >> 8))
    return false;

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  if (mbedtls_sha256_starts(&sha, /*is224=*/0) != 0 || mbedtls_sha256_update(&sha, header, sizeof(header)) != 0) {
    mbedtls_sha256_free(&sha);
    return false;
  }

  size_t offset = sizeof(header);
  uint8_t checksum = 0xEF;
  for (uint8_t segment = 0; segment < header[1]; ++segment) {
    uint8_t segmentHeader[8];
    if (offset > partition->size || sizeof(segmentHeader) > partition->size - offset ||
        esp_partition_read(partition, offset, segmentHeader, sizeof(segmentHeader)) != ESP_OK ||
        mbedtls_sha256_update(&sha, segmentHeader, sizeof(segmentHeader)) != 0) {
      mbedtls_sha256_free(&sha);
      return false;
    }
    if (segment == 0) {
      uint32_t loadAddress = 0;
      memcpy(&loadAddress, segmentHeader, sizeof(loadAddress));
      const bool appFlashSegment = (loadAddress >= 0x3C000000u && loadAddress < 0x3E000000u) ||
                                   (loadAddress >= 0x42000000u && loadAddress < 0x44000000u);
      if (!appFlashSegment) {
        mbedtls_sha256_free(&sha);
        return false;
      }
    }
    uint32_t length = 0;
    memcpy(&length, segmentHeader + 4, sizeof(length));
    offset += sizeof(segmentHeader);
    if (length > partition->size - offset) {
      mbedtls_sha256_free(&sha);
      return false;
    }
    size_t remaining = length;
    while (remaining > 0) {
      const size_t amount = remaining < sizeof(appValidationBuffer) ? remaining : sizeof(appValidationBuffer);
      if (esp_partition_read(partition, offset, appValidationBuffer, amount) != ESP_OK ||
          mbedtls_sha256_update(&sha, appValidationBuffer, amount) != 0) {
        mbedtls_sha256_free(&sha);
        return false;
      }
      for (size_t i = 0; i < amount; ++i) checksum ^= appValidationBuffer[i];
      offset += amount;
      remaining -= amount;
    }
  }
  const size_t padEnd = (offset + 16) & ~static_cast<size_t>(15);
  const size_t expectedSize = padEnd + (header[23] ? 32 : 0);
  if (padEnd > partition->size || expectedSize > partition->size || padEnd == offset) {
    mbedtls_sha256_free(&sha);
    return false;
  }
  uint8_t padding[16];
  const size_t padLength = padEnd - offset;
  if (esp_partition_read(partition, offset, padding, padLength) != ESP_OK || padding[padLength - 1] != checksum ||
      mbedtls_sha256_update(&sha, padding, padLength) != 0) {
    mbedtls_sha256_free(&sha);
    return false;
  }
  if (header[23]) {
    uint8_t computed[32];
    uint8_t stored[32];
    const bool hashOk = mbedtls_sha256_finish(&sha, computed) == 0 &&
                        esp_partition_read(partition, padEnd, stored, sizeof(stored)) == ESP_OK &&
                        memcmp(computed, stored, sizeof(computed)) == 0;
    mbedtls_sha256_free(&sha);
    return hashOk;
  }
  mbedtls_sha256_free(&sha);
  return true;
}
#endif
}  // namespace

bool entryState(const esp_partition_t* partition, uint32_t& state) {
  if (!partition) return false;
  const uint32_t rawIndex =
      static_cast<uint32_t>(partition->subtype) - static_cast<uint32_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  if (rawIndex > 1) return false;
  const esp_partition_t* otadata = findOtadata();
  uint8_t model[kModelSize];
  if (!readModel(otadata, model)) return false;
  Selection selected{};
  if (!entryForPartition(model, sizeof(model), static_cast<uint8_t>(rawIndex), selected)) return false;
  state = selected.state;
  return true;
}

bool switchTo(const esp_partition_t* dest, uint32_t state) {
  if (!dest) return false;
  // ota_seq encoding: (seq - 1) % NUM_OTA_PARTITIONS picks the partition.
  const uint32_t destOtaIdx =
      static_cast<uint32_t>(dest->subtype) - static_cast<uint32_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  if (destOtaIdx > 1) {
    LOG_ERR("BOOT", "dest is not an OTA app partition (subtype=0x%02X)", dest->subtype);
    return false;
  }
  const esp_partition_t* otadata = findOtadata();
  uint8_t model[kModelSize];
  uint8_t next[kSelectEntrySize];
  int targetSlot = 0;
  if (!readModel(otadata, model) ||
      !prepareSwitch(model, sizeof(model), static_cast<uint8_t>(destOtaIdx), state, next, targetSlot))
    return false;
  Selection active{};
  const bool hasActive = selectedEntry(model, sizeof(model), active);
  // Write to the OTHER slot (so the bootloader sees a higher seq there).
  const size_t targetOff = static_cast<size_t>(targetSlot) * kOtaSectorSize;

  if (esp_partition_erase_range(otadata, targetOff, kOtaSectorSize) != ESP_OK) {
    LOG_ERR("BOOT", "otadata erase failed (slot=%d)", targetSlot);
    return false;
  }
  if (esp_partition_write(otadata, targetOff, next, sizeof(next)) != ESP_OK) {
    LOG_ERR("BOOT", "otadata write failed (slot=%d)", targetSlot);
    return false;
  }

  SelectEntry written{};
  memcpy(&written, next, sizeof(written));
  LOG_INF("BOOT", "otadata: active slot=%d seq=%u; wrote slot=%d seq=%u state=%u -> %s", hasActive ? active.slot : -1,
          static_cast<unsigned>(hasActive ? active.seq : 0), targetSlot, static_cast<unsigned>(written.otaSeq),
          static_cast<unsigned>(written.otaState), dest->label);
  return true;
}

void selectFallbackForUnconfirmedX4Pro() {
#if FREEINK_DEVICE_X4PRO
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED) return;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) return;
  uint32_t state = 0xFFFFFFFFu;
  if (!entryState(running, state) || state == kOtaImgValid) return;

  const uint32_t runningIndex =
      static_cast<uint32_t>(running->subtype) - static_cast<uint32_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  if (runningIndex > 1) return;
  const auto otherSubtype = static_cast<esp_partition_subtype_t>(
      static_cast<uint32_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0) + (1u - runningIndex));
  const esp_partition_t* other = esp_partition_find_first(ESP_PARTITION_TYPE_APP, otherSubtype, nullptr);
  uint16_t chipId = 0xFFFF;
  if (esp_partition_read(running, 12, &chipId, sizeof(chipId)) != ESP_OK || !hasValidImageHeader(other, chipId)) {
    LOG_INF("BOOT", "unconfirmed X4 Pro has no valid fallback image");
    return;
  }
  if (switchTo(other, kOtaImgValid)) {
    LOG_INF("BOOT", "unconfirmed X4 Pro; selected fallback %s until Home renders", other->label);
  }
#endif
}

void confirmRunningX4ProAfterHome() {
#if FREEINK_DEVICE_X4PRO
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) return;
  uint32_t state = 0xFFFFFFFFu;
  if (entryState(running, state) && state == kOtaImgValid) return;
  if (!switchTo(running, kOtaImgValid)) {
    LOG_ERR("BOOT", "failed to mark running X4 Pro slot VALID after Home render");
    return;
  }
  // The Arduino SDK marks the currently selected otadata entry. Select the
  // running slot first so the SDK cannot mark the fallback slot instead.
  esp_ota_mark_app_valid_cancel_rollback();
#endif
}

}  // namespace ota_boot
