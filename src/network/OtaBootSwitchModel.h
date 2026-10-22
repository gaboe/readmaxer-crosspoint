#pragma once

#include <cstddef>
#include <cstdint>

namespace ota_boot {

constexpr size_t kSelectEntrySize = 32;
constexpr size_t kModelSize = 2 * kSelectEntrySize;
constexpr uint32_t kOtaImgNew = 0;
constexpr uint32_t kOtaImgValid = 1;
constexpr uint32_t kOtaImgInvalid = 3;
constexpr uint32_t kOtaImgAborted = 4;

struct SelectEntry {
  uint32_t otaSeq;
  uint8_t seqLabel[20];
  uint32_t otaState;
  uint32_t crc;
};
static_assert(sizeof(SelectEntry) == kSelectEntrySize, "SelectEntry must be 32 bytes");

struct Selection {
  int slot;
  uint32_t seq;
  uint8_t partitionIndex;
  uint32_t state;
};

uint32_t computeSeqCrc(uint32_t seq);
bool selectedEntry(const uint8_t* model, size_t modelSize, Selection& selection);
bool entryForPartition(const uint8_t* model, size_t modelSize, uint8_t partitionIndex, Selection& selection);
bool prepareSwitch(const uint8_t* model, size_t modelSize, uint8_t partitionIndex, uint32_t state,
                   uint8_t nextEntry[kSelectEntrySize], int& targetSlot);

}  // namespace ota_boot
