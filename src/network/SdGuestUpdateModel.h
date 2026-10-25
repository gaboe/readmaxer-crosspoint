#pragma once

#include <cstddef>
#include <cstdint>

namespace sd_guest_update {

enum class Result {
  Ok,
  BadManifest,
  WrongDevice,
  BadSignature,
  NoMemory,
  TooLarge,
  MissingImage,
  WrongSize,
  WrongSha,
  AlreadyInstalled,
  InstallFailed,
};

struct Manifest {
  char shaHex[65];
  uint8_t sha[32];
  uint32_t size;
};

const char* resultName(Result result);
Result parseAndVerify(const uint8_t* json, size_t length, const char* deviceId, uint32_t maximumSize,
                      Manifest& manifest);
Result parseAndVerifyWithKey(const uint8_t* json, size_t length, const char* deviceId, uint32_t maximumSize,
                             const uint8_t (&publicKey)[32], Manifest& manifest);

}  // namespace sd_guest_update
