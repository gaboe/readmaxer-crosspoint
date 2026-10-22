#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "network/OtaBootSwitchModel.h"

namespace {
using ota_boot::SelectEntry;

void putEntry(std::array<uint8_t, ota_boot::kModelSize>& model, int slot, uint32_t seq, uint32_t state,
              bool validCrc = true) {
  SelectEntry entry{};
  entry.otaSeq = seq;
  std::memset(entry.seqLabel, 0xFF, sizeof(entry.seqLabel));
  entry.otaState = state;
  entry.crc = validCrc ? ota_boot::computeSeqCrc(seq) : 0;
  std::memcpy(model.data() + slot * ota_boot::kSelectEntrySize, &entry, sizeof(entry));
}
}  // namespace

TEST(OtaBootSwitchModel, ChoosesHighestValidSequenceAndSkipsInvalidStates) {
  std::array<uint8_t, ota_boot::kModelSize> model;
  model.fill(0xFF);
  putEntry(model, 0, 3, ota_boot::kOtaImgValid);
  putEntry(model, 1, 4, ota_boot::kOtaImgInvalid);

  ota_boot::Selection selected{};
  ASSERT_TRUE(ota_boot::selectedEntry(model.data(), model.size(), selected));
  EXPECT_EQ(selected.slot, 0);
  EXPECT_EQ(selected.seq, 3u);
  EXPECT_EQ(selected.partitionIndex, 0);
  EXPECT_EQ(selected.state, ota_boot::kOtaImgValid);
}

TEST(OtaBootSwitchModel, UsesEspIdfOtaSequenceCrc) { EXPECT_EQ(ota_boot::computeSeqCrc(1), 0x66074786u); }

TEST(OtaBootSwitchModel, SkipsBadCrcAndAbortedEntries) {
  std::array<uint8_t, ota_boot::kModelSize> model;
  model.fill(0xFF);
  putEntry(model, 0, 7, ota_boot::kOtaImgAborted);
  putEntry(model, 1, 8, ota_boot::kOtaImgValid, false);

  ota_boot::Selection selected{};
  EXPECT_FALSE(ota_boot::selectedEntry(model.data(), model.size(), selected));
}

TEST(OtaBootSwitchModel, WritesOtherEntryWithNextSequenceAndRequestedState) {
  std::array<uint8_t, ota_boot::kModelSize> model;
  model.fill(0xFF);
  putEntry(model, 0, 9, ota_boot::kOtaImgNew);
  uint8_t next[ota_boot::kSelectEntrySize]{};
  int targetSlot = -1;

  ASSERT_TRUE(ota_boot::prepareSwitch(model.data(), model.size(), 1, ota_boot::kOtaImgValid, next, targetSlot));
  SelectEntry written{};
  std::memcpy(&written, next, sizeof(written));
  EXPECT_EQ(targetSlot, 1);
  EXPECT_EQ(written.otaSeq, 10u);
  EXPECT_EQ(written.otaState, ota_boot::kOtaImgValid);
  EXPECT_EQ(written.crc, ota_boot::computeSeqCrc(10));
}

TEST(OtaBootSwitchModel, FindsUnconfirmedEntryByPartition) {
  std::array<uint8_t, ota_boot::kModelSize> model;
  model.fill(0xFF);
  putEntry(model, 0, 12, ota_boot::kOtaImgNew);
  putEntry(model, 1, 13, ota_boot::kOtaImgValid);

  ota_boot::Selection selected{};
  ASSERT_TRUE(ota_boot::entryForPartition(model.data(), model.size(), 1, selected));
  EXPECT_EQ(selected.seq, 12u);
  EXPECT_EQ(selected.state, ota_boot::kOtaImgNew);
}
