#include "GuestInstallModel.h"

#include <algorithm>
#include <cstring>

#include "FirmwareHostTag.h"
#include "GuestSlotModel.h"

namespace guest_install {
namespace {
class DeviceScanner {
 public:
  explicit DeviceScanner(const char* mac) : mac(mac) {}

  void feed(const uint8_t* data, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
      const char c = static_cast<char>(data[i]);
      if (remaining) {
        if (c != mac[12 - remaining]) mismatch = true;
        --remaining;
      }
      // A new prefix inside a malformed marker must also be refused.
      switch (prefix) {
        case 0:
          prefix = c == 'S' ? 1 : 0;
          break;
        case 1:
          prefix = c == 'R' ? 2 : (c == 'S' ? 1 : 0);
          break;
        case 2:
          prefix = c == 'S' ? 1 : 0;
          if (c == '_') {
            if (count < 2) ++count;
            remaining = 12;
          }
          break;
      }
    }
  }

  bool valid() const { return count == 1 && !mismatch && remaining == 0; }

 private:
  const char* mac;
  uint8_t prefix = 0;
  uint8_t remaining = 0;
  uint8_t count = 0;
  bool mismatch = false;
};

int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

const char* resultName(Result result) {
  switch (result) {
    case Result::Ok:
      return "OK";
    case Result::BadManifest:
      return "BAD_MANIFEST";
    case Result::WrongSha:
      return "WRONG_SHA";
    case Result::WrongProject:
      return "WRONG_PROJECT";
    case Result::BadImage:
      return "BAD_IMAGE";
    case Result::BadIdentity:
      return "BAD_IDENTITY";
    case Result::TooLarge:
      return "TOO_LARGE";
    case Result::ReadFailed:
      return "READ_FAILED";
    case Result::NoMemory:
      return "NO_MEMORY";
    case Result::NoPartition:
      return "NO_PARTITION";
    case Result::GuardRefused:
      return "GUARD_REFUSED";
    case Result::WriteFailed:
      return "WRITE_FAILED";
    case Result::BadReadback:
      return "BAD_READBACK";
    case Result::SelectionFailed:
      return "SELECTION_FAILED";
    case Result::SelectionUnknown:
      return "SELECTION_UNKNOWN";
  }
  return "UNKNOWN";
}

bool parseSha(const char* text, uint8_t (&digest)[32]) {
  if (!text || std::strlen(text) != 64) return false;
  for (unsigned i = 0; i < 32; ++i) {
    const int hi = hex(text[2 * i]);
    const int lo = hex(text[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    digest[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

Result validate(ota_boot::SlotReader read, void* ctx, uint32_t size, uint32_t destinationSize, const char* ownMac,
                const uint8_t (&expectedSha)[32], const ota_boot::ImageHasher& imageHash,
                const ota_boot::ImageHasher& fileHash, uint8_t* buffer, uint32_t bufferSize) {
  if (size > destinationSize) return Result::TooLarge;
  uint32_t checkedSize = 0;
  if (ota_boot::checkAppImage(read, ctx, size, 9, imageHash, buffer, bufferSize, nullptr, nullptr, &checkedSize) !=
          ota_boot::ImageCheck::Ok ||
      checkedSize != size)
    return Result::BadImage;
  if (!guest_slot::inspectHeader(read, ctx, size).installed) return Result::WrongProject;
  if (!ownMac || std::strlen(ownMac) != 12) return Result::BadIdentity;
  guest_slot::Scanner guest;
  host_tag::Scanner host;
  DeviceScanner device(ownMac);
  for (uint32_t pos = 0; pos < size;) {
    const uint32_t len = std::min(bufferSize, size - pos);
    if (!read(pos, buffer, len, ctx) || !fileHash.update(fileHash.ctx, buffer, len)) return Result::ReadFailed;
    guest.feed(buffer, len);
    host.feed(buffer, len);
    device.feed(buffer, len);
    pos += len;
  }
  uint8_t digest[32];
  if (!fileHash.finish(fileHash.ctx, digest)) return Result::ReadFailed;
  if (std::memcmp(digest, expectedSha, sizeof(digest)) != 0) return Result::WrongSha;
  if (!guest.exactlyOne() || host.count() != 0 || !device.valid()) return Result::BadIdentity;
  return Result::Ok;
}

}  // namespace guest_install
