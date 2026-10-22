#include "OtaBootSwitchModel.h"

#include <cstring>

namespace ota_boot {

namespace {
bool readEntry(const uint8_t* model, size_t modelSize, int slot, SelectEntry& entry) {
  if (!model || slot < 0 || slot > 1 || modelSize < kModelSize) return false;
  std::memcpy(&entry, model + static_cast<size_t>(slot) * kSelectEntrySize, sizeof(entry));
  return true;
}

bool usable(const SelectEntry& entry) {
  return entry.otaSeq != 0xFFFFFFFFu && entry.crc == computeSeqCrc(entry.otaSeq) && entry.otaState != kOtaImgInvalid &&
         entry.otaState != kOtaImgAborted;
}

uint8_t partitionForSeq(uint32_t seq) { return static_cast<uint8_t>((seq - 1u) % 2u); }
}  // namespace

uint32_t computeSeqCrc(uint32_t seq) {
  uint32_t crc = UINT32_MAX;
  for (unsigned i = 0; i < 4; ++i) {
    crc ^= static_cast<uint8_t>(seq >> (i * 8));
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

bool selectedEntry(const uint8_t* model, size_t modelSize, Selection& selection) {
  if (!model || modelSize < kModelSize) return false;
  bool found = false;
  for (int slot = 0; slot < 2; ++slot) {
    SelectEntry entry{};
    readEntry(model, modelSize, slot, entry);
    if (!usable(entry) || (found && entry.otaSeq <= selection.seq)) continue;
    selection = {slot, entry.otaSeq, partitionForSeq(entry.otaSeq), entry.otaState};
    found = true;
  }
  return found;
}

bool entryForPartition(const uint8_t* model, size_t modelSize, uint8_t partitionIndex, Selection& selection) {
  if (!model || modelSize < kModelSize || partitionIndex > 1) return false;
  bool found = false;
  for (int slot = 0; slot < 2; ++slot) {
    SelectEntry entry{};
    readEntry(model, modelSize, slot, entry);
    if (entry.otaSeq == 0xFFFFFFFFu || entry.crc != computeSeqCrc(entry.otaSeq) ||
        partitionForSeq(entry.otaSeq) != partitionIndex || (found && entry.otaSeq <= selection.seq))
      continue;
    selection = {slot, entry.otaSeq, partitionIndex, entry.otaState};
    found = true;
  }
  return found;
}

bool prepareSwitch(const uint8_t* model, size_t modelSize, uint8_t partitionIndex, uint32_t state,
                   uint8_t nextEntry[kSelectEntrySize], int& targetSlot) {
  if (!model || modelSize < kModelSize || !nextEntry || partitionIndex > 1) return false;
  Selection active{-1, 0, 0, 0};
  const bool hasActive = selectedEntry(model, modelSize, active);
  uint32_t nextSeq = (hasActive ? active.seq : 0) + 1;
  while (partitionForSeq(nextSeq) != partitionIndex) ++nextSeq;

  SelectEntry entry{};
  entry.otaSeq = nextSeq;
  std::memset(entry.seqLabel, 0xFF, sizeof(entry.seqLabel));
  entry.otaState = state;
  entry.crc = computeSeqCrc(entry.otaSeq);
  std::memcpy(nextEntry, &entry, sizeof(entry));
  targetSlot = active.slot == 0 ? 1 : 0;
  return true;
}

}  // namespace ota_boot
