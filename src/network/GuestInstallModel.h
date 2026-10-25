#pragma once

#include "OtaSlotModel.h"

namespace guest_install {

enum class Result {
  Ok,
  BadManifest,
  WrongSha,
  WrongProject,
  BadImage,
  BadIdentity,
  TooLarge,
  WrongSize,
  ReadFailed,
  NoMemory,
  NoPartition,
  GuardRefused,
  WriteFailed,
  BadReadback,
  SelectionFailed,
  SelectionUnknown,
};

const char* resultName(Result result);
bool parseSha(const char* text, uint8_t (&digest)[32]);

// Both hash contexts must be started by the caller. The shared CP2 checker owns image-format rules.
Result validate(ota_boot::SlotReader read, void* ctx, uint32_t size, uint32_t destinationSize, const char* ownMac,
                const uint8_t (&expectedSha)[32], const ota_boot::ImageHasher& imageHash,
                const ota_boot::ImageHasher& fileHash, uint8_t* buffer, uint32_t bufferSize);

}  // namespace guest_install
