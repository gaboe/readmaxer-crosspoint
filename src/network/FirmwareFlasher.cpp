#include "FirmwareFlasher.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <spi_flash_mmap.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "FirmwareBoardTag.h"
#include "FirmwareHostTag.h"
#include "GuestSlot.h"
#include "GuestSlotModel.h"
#include "OtaBootSwitch.h"
#include "OtaSlotModel.h"

namespace firmware_flash {

namespace {
constexpr size_t MIN_FIRMWARE_SIZE = 64 * 1024;
constexpr size_t SEC = SPI_FLASH_SEC_SIZE;  // 4 KiB
constexpr size_t BLK = 64 * 1024;           // 64 KiB block-erase granularity
constexpr size_t CHUNK = 4096;
constexpr size_t SHA_TRAILER = 32;
}  // namespace

const char* resultName(Result r) {
  switch (r) {
    case Result::OK:
      return "OK";
    case Result::OPEN_FAIL:
      return "OPEN_FAIL";
    case Result::TOO_SMALL:
      return "TOO_SMALL";
    case Result::TOO_LARGE:
      return "TOO_LARGE";
    case Result::BAD_MAGIC:
      return "BAD_MAGIC";
    case Result::BAD_SEGMENTS:
      return "BAD_SEGMENTS";
    case Result::BAD_CHECKSUM:
      return "BAD_CHECKSUM";
    case Result::BAD_SHA:
      return "BAD_SHA";
    case Result::BAD_CHIP:
      return "BAD_CHIP";
    case Result::WRONG_BOARD:
      return "WRONG_BOARD";
    case Result::WRONG_FORK:
      return "WRONG_FORK";
    case Result::GUEST_IMAGE:
      return "GUEST_IMAGE";
    case Result::BAD_SIZE:
      return "BAD_SIZE";
    case Result::NO_PARTITION:
      return "NO_PARTITION";
    case Result::OOM:
      return "OOM";
    case Result::READ_FAIL:
      return "READ_FAIL";
    case Result::ERASE_FAIL:
      return "ERASE_FAIL";
    case Result::WRITE_FAIL:
      return "WRITE_FAIL";
    case Result::OTADATA_FAIL:
      return "OTADATA_FAIL";
    case Result::OTADATA_UNKNOWN:
      return "OTADATA_UNKNOWN";
    case Result::CONFIRM_REQUIRED:
      return "CONFIRM_REQUIRED";
    case Result::GUARD_REFUSED:
      return "GUARD_REFUSED";
    case Result::BAD_READBACK:
      return "BAD_READBACK";
  }
  return "?";
}

uint16_t runningPartitionChipId() {
  // esp_partition_read hits SPI flash; cache the running slot's chip_id so we
  // only pay that cost once per boot. The running image is immutable at
  // runtime, so a function-local static is safe here.
  static uint16_t cached = [] {
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (!run) return static_cast<uint16_t>(0xFFFF);
    uint16_t id = 0xFFFF;
    // chip_id sits at offset 12 of esp_image_header_t. memcpy target is a
    // uint16_t local, so RISC-V alignment is guaranteed.
    if (esp_partition_read(run, 12, &id, sizeof(id)) != ESP_OK) return static_cast<uint16_t>(0xFFFF);
    return id;
  }();
  return cached;
}

Result validateImageFile(const char* sdPath, size_t partitionSize) {
  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) {
    LOG_ERR("FLASH", "validate: open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }

  const size_t fileSize = file.fileSize();
  if (fileSize < MIN_FIRMWARE_SIZE) {
    LOG_ERR("FLASH", "validate: too small: %u", static_cast<unsigned>(fileSize));
    file.close();
    return Result::TOO_SMALL;
  }
  if (partitionSize > 0 && fileSize > partitionSize) {
    LOG_ERR("FLASH", "validate: too large: %u > %u", static_cast<unsigned>(fileSize),
            static_cast<unsigned>(partitionSize));
    file.close();
    return Result::TOO_LARGE;
  }

  auto buf = makeUniqueNoThrow<uint8_t[]>(CHUNK);
  if (!buf) {
    LOG_ERR("FLASH", "OOM: image validation buffer (%u bytes)", static_cast<unsigned>(CHUNK));
    return Result::OOM;
  }
  struct Markers {
    board_tag::Scanner board;
    host_tag::Scanner host;
    guest_slot::Scanner guest;
  } markers;
  const auto visit = [](const uint8_t* bytes, uint32_t len, void* ctx) {
    auto& markers = *static_cast<Markers*>(ctx);
    markers.board.feed(bytes, len);
    markers.host.feed(bytes, len);
    markers.guest.feed(bytes, len);
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
  const auto read = [](uint32_t offset, void* dst, uint32_t len, void* ctx) {
    auto& source = *static_cast<HalFile*>(ctx);
    return source.seek(offset) && source.read(static_cast<uint8_t*>(dst), len) == static_cast<int>(len);
  };
  uint32_t imageSize = 0;
  const auto check =
      mbedtls_sha256_starts(&sha, 0) == 0
          ? ota_boot::checkAppImage(read, &file, static_cast<uint32_t>(fileSize), runningPartitionChipId(), hasher,
                                    buf.get(), CHUNK, visit, &markers, &imageSize)
          : ota_boot::ImageCheck::BadSha;
  mbedtls_sha256_free(&sha);
  file.close();
  if (check != ota_boot::ImageCheck::Ok)
    LOG_ERR("FLASH", "validate: image check failed (%u)", static_cast<unsigned>(check));
  switch (check) {
    case ota_boot::ImageCheck::ReadError:
      return Result::READ_FAIL;
    case ota_boot::ImageCheck::BadMagic:
      return Result::BAD_MAGIC;
    case ota_boot::ImageCheck::BadChip:
      return Result::BAD_CHIP;
    case ota_boot::ImageCheck::BadSegments:
      return Result::BAD_SEGMENTS;
    case ota_boot::ImageCheck::BadChecksum:
      return Result::BAD_CHECKSUM;
    case ota_boot::ImageCheck::BadSha:
      return Result::BAD_SHA;
    case ota_boot::ImageCheck::Ok:
      break;
  }
  if (imageSize != fileSize) return Result::BAD_SIZE;
#if FREEINK_DEVICE_X4PRO
  const auto identity = host_tag::classify(markers.board.matched(), markers.board.mismatch(), markers.host.count(),
                                           markers.guest.count());
  switch (identity) {
    case host_tag::Identity::Host:
      return Result::OK;
    case host_tag::Identity::Guest:
      return Result::GUEST_IMAGE;
    case host_tag::Identity::WrongBoard:
      return Result::WRONG_BOARD;
    case host_tag::Identity::WrongFork:
      return Result::WRONG_FORK;
  }
#else
  if (markers.board.mismatch()) return Result::WRONG_BOARD;
#endif
  return Result::OK;
}

Result inspectImageFile(const char* sdPath, size_t* imageSize) {
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) return Result::NO_PARTITION;

  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) return Result::OPEN_FAIL;
  const size_t size = file.fileSize();
  file.close();

  const Result result = validateImageFile(sdPath, dest->size);
  if (result == Result::OK && imageSize) *imageSize = size;
  return result;
}

namespace {
// SHA-256 of the first `size` bytes of `sdPath`, read in CHUNK pieces through `buf`.
bool hashFile(const char* sdPath, size_t size, uint8_t* buf, uint8_t (&out)[SHA_TRAILER]) {
  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) return false;
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, /*is224=*/0);
  bool ok = true;
  for (size_t pos = 0; ok && pos < size; pos += CHUNK) {
    const size_t want = std::min<size_t>(CHUNK, size - pos);
    ok = file.read(buf, want) == static_cast<int>(want) && mbedtls_sha256_update(&sha, buf, want) == 0;
  }
  mbedtls_sha256_finish(&sha, out);
  mbedtls_sha256_free(&sha);
  file.close();
  return ok;
}

// SHA-256 of the first `size` bytes of `part`, read back from flash.
bool hashPartition(const esp_partition_t* part, size_t size, uint8_t* buf, uint8_t (&out)[SHA_TRAILER]) {
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, /*is224=*/0);
  bool ok = true;
  for (size_t pos = 0; ok && pos < size; pos += CHUNK) {
    const size_t want = std::min<size_t>(CHUNK, size - pos);
    ok = esp_partition_read(part, pos, buf, want) == ESP_OK;
    if (ok) mbedtls_sha256_update(&sha, buf, want);
  }
  mbedtls_sha256_finish(&sha, out);
  mbedtls_sha256_free(&sha);
  return ok;
}
}  // namespace

Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx) {
  // Resolve destination first so we can size-check during validation. The full image-integrity
  // pass below verifies header, segment table, XOR checksum and SHA256 trailer end-to-end before
  // we touch otadata, so a truncated/corrupted .bin can never become the next boot target.
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    LOG_ERR("FLASH", "no next-update partition");
    return Result::NO_PARTITION;
  }

  const Result validateRes = validateImageFile(sdPath, dest->size);
  if (validateRes != Result::OK) {
    LOG_ERR("FLASH", "image validation failed: %s", resultName(validateRes));
    return validateRes;
  }

  // While the boot fallback is armed, otadata selects `dest`. Select the
  // running slot first so an interrupted write never leaves a partial image selected.
  switch (ota_boot::keepRunningSlot()) {
    case ota_boot::SelectionResult::Selected:
      break;
    case ota_boot::SelectionResult::Failed:
      return Result::OTADATA_FAIL;
    case ota_boot::SelectionResult::Unknown:
      return Result::OTADATA_UNKNOWN;
  }
  guest_slot::invalidateCache();

  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) {
    LOG_ERR("FLASH", "open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }

  const size_t firmwareSize = file.fileSize();
  LOG_INF("FLASH", "src=%s size=%u dest=%s @0x%x partsize=%u", sdPath, static_cast<unsigned>(firmwareSize), dest->label,
          static_cast<unsigned>(dest->address), static_cast<unsigned>(dest->size));

  auto buffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[CHUNK]);
  if (!buffer) {
    LOG_ERR("FLASH", "OOM");
    file.close();
    return Result::OOM;
  }

  {
    ota_boot::AppWriteGuard appWrite(dest);
    if (!appWrite) return ota_boot::confirmationPending() ? Result::CONFIRM_REQUIRED : Result::GUARD_REFUSED;

    // Interleave erase + write so the progress bar advances 0→100% smoothly
    // rather than stalling for several seconds during a single up-front erase.
    size_t streamPos = 0;
    size_t erasedUpto = 0;
    while (streamPos < firmwareSize) {
      if (streamPos >= erasedUpto) {
        size_t eraseLen = std::min<size_t>(BLK, dest->size - streamPos);
        eraseLen = (eraseLen + SEC - 1) & ~(SEC - 1);
        eraseLen = std::min<size_t>(eraseLen, dest->size - streamPos);
        if (esp_partition_erase_range(dest, streamPos, eraseLen) != ESP_OK) {
          LOG_ERR("FLASH", "erase @%u (len=%u) failed", static_cast<unsigned>(streamPos),
                  static_cast<unsigned>(eraseLen));
          file.close();
          return Result::ERASE_FAIL;
        }
        erasedUpto = streamPos + eraseLen;
      }

      const size_t want = std::min<size_t>(CHUNK, firmwareSize - streamPos);
      const int read = file.read(buffer.get(), want);
      if (read <= 0 || static_cast<size_t>(read) != want) {
        LOG_ERR("FLASH", "read @%u: got=%d want=%u", static_cast<unsigned>(streamPos), read,
                static_cast<unsigned>(want));
        file.close();
        return Result::READ_FAIL;
      }
      if (esp_partition_write(dest, streamPos, buffer.get(), want) != ESP_OK) {
        LOG_ERR("FLASH", "write @%u failed", static_cast<unsigned>(streamPos));
        file.close();
        return Result::WRITE_FAIL;
      }
      streamPos += want;
      if (onProgress) onProgress(streamPos, firmwareSize, ctx);
      delay(1);
    }
    file.close();

    // A fresh file read, not the bytes just written: an SD read error inside the
    // write loop would otherwise be hashed and written alike.
    uint8_t fileSha[SHA_TRAILER];
    uint8_t slotSha[SHA_TRAILER];
    if (!hashFile(sdPath, firmwareSize, buffer.get(), fileSha) ||
        !hashPartition(dest, firmwareSize, buffer.get(), slotSha)) {
      LOG_ERR("FLASH", "readback read failed");
      return Result::READ_FAIL;
    }
    if (std::memcmp(fileSha, slotSha, SHA_TRAILER) != 0) {
      LOG_ERR("FLASH", "readback SHA-256 mismatch; otadata unchanged");
      return Result::BAD_READBACK;
    }
  }
  switch (ota_boot::switchTo(dest)) {
    case ota_boot::SelectionResult::Selected:
      return Result::OK;
    case ota_boot::SelectionResult::Failed:
      return Result::OTADATA_FAIL;
    case ota_boot::SelectionResult::Unknown:
      return Result::OTADATA_UNKNOWN;
  }
  return Result::OTADATA_FAIL;
}

}  // namespace firmware_flash
