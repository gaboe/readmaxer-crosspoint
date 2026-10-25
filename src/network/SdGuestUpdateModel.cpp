#include "SdGuestUpdateModel.h"

#include <ArduinoJson.h>
#include <monocypher-ed25519.h>

#include <cstdio>
#include <cstring>

#include "GuestInstallModel.h"

namespace sd_guest_update {
namespace {
constexpr uint8_t pinnedPublicKey[32] = {0x91, 0xe4, 0xfe, 0x88, 0x02, 0xce, 0x03, 0xd4, 0x1c, 0xb8, 0xfd,
                                         0x63, 0x1a, 0xd2, 0x8f, 0x61, 0xf1, 0x6d, 0x30, 0xde, 0xa3, 0x92,
                                         0x4b, 0x59, 0x92, 0x36, 0x8d, 0xc8, 0xe7, 0xfb, 0xa1, 0xa0};

int base64Digit(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool decodeSignature(const char* text, uint8_t (&out)[64]) {
  if (!text || std::strlen(text) != 88 || text[86] != '=' || text[87] != '=') return false;
  for (size_t i = 0; i < 21; ++i) {
    const int a = base64Digit(text[4 * i]);
    const int b = base64Digit(text[4 * i + 1]);
    const int c = base64Digit(text[4 * i + 2]);
    const int d = base64Digit(text[4 * i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return false;
    out[3 * i] = static_cast<uint8_t>((a << 2) | (b >> 4));
    out[3 * i + 1] = static_cast<uint8_t>((b << 4) | (c >> 2));
    out[3 * i + 2] = static_cast<uint8_t>((c << 6) | d);
  }
  const int a = base64Digit(text[84]);
  const int b = base64Digit(text[85]);
  if (a < 0 || b < 0 || (b & 15) != 0) return false;
  out[63] = static_cast<uint8_t>((a << 2) | (b >> 4));
  return true;
}

bool lowercaseHex(const char* text, size_t length) {
  if (!text || std::strlen(text) != length) return false;
  for (size_t i = 0; i < length; ++i)
    if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
  return true;
}
}  // namespace

const char* resultName(Result result) {
  switch (result) {
    case Result::Ok:
      return "OK";
    case Result::BadManifest:
      return "BAD_MANIFEST";
    case Result::WrongDevice:
      return "WRONG_DEVICE";
    case Result::BadSignature:
      return "BAD_SIGNATURE";
    case Result::NoMemory:
      return "NO_MEMORY";
    case Result::TooLarge:
      return "TOO_LARGE";
    case Result::MissingImage:
      return "MISSING_IMAGE";
    case Result::WrongSize:
      return "WRONG_SIZE";
    case Result::WrongSha:
      return "WRONG_SHA";
    case Result::AlreadyInstalled:
      return "ALREADY_INSTALLED";
    case Result::InstallFailed:
      return "INSTALL_FAILED";
  }
  return "UNKNOWN";
}

Result parseAndVerifyWithKey(const uint8_t* json, size_t length, const char* deviceId, uint32_t maximumSize,
                             const uint8_t (&publicKey)[32], Manifest& manifest) {
  if (!json || length == 0 || length > 512 || !deviceId) return Result::BadManifest;
  JsonDocument doc;
  if (deserializeJson(doc, json, length) || !doc.is<JsonObject>() || !doc["v"].is<int>() || doc["v"].as<int>() != 1 ||
      !doc["device_id"].is<const char*>() || !doc["sha256"].is<const char*>() || !doc["size"].is<uint32_t>() ||
      !doc["version"].is<const char*>() || !doc["sig"].is<const char*>())
    return Result::BadManifest;
  const char* id = doc["device_id"];
  const char* sha = doc["sha256"];
  const char* version = doc["version"];
  const uint32_t size = doc["size"].as<uint32_t>();
  if (!lowercaseHex(sha, 64) || !lowercaseHex(version, 40) || size == 0 || std::strlen(id) != 15 ||
      std::strncmp(id, "sr-", 3) != 0 || !lowercaseHex(id + 3, 12) || !guest_install::parseSha(sha, manifest.sha))
    return Result::BadManifest;
  if (std::strcmp(id, deviceId)) return Result::WrongDevice;
  if (size > maximumSize) return Result::TooLarge;
  uint8_t signature[64];
  if (!decodeSignature(doc["sig"], signature)) return Result::BadManifest;
  char message[192];
  const int messageLength = std::snprintf(message, sizeof(message), "readmaxer-guest-update-v1\n%s\n%s\n%lu\n%s\n", id,
                                          sha, static_cast<unsigned long>(size), version);
  if (messageLength < 0 || static_cast<size_t>(messageLength) >= sizeof(message)) return Result::BadManifest;
  if (crypto_ed25519_check(signature, publicKey, reinterpret_cast<const uint8_t*>(message), messageLength))
    return Result::BadSignature;
  std::memcpy(manifest.shaHex, sha, 65);
  manifest.size = size;
  return Result::Ok;
}

Result parseAndVerify(const uint8_t* json, size_t length, const char* deviceId, uint32_t maximumSize,
                      Manifest& manifest) {
  return parseAndVerifyWithKey(json, length, deviceId, maximumSize, pinnedPublicKey, manifest);
}
}  // namespace sd_guest_update
