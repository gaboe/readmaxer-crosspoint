#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "OtaSlotModel.h"

using namespace ota_boot;

namespace {
using Otadata = SelectEntry[kEntrySectors];
SelectEntry blank() {
  SelectEntry e;
  std::memset(&e, 0xff, sizeof(e));
  return e;
}
SelectEntry entry(uint32_t seq, uint32_t state) {
  auto e = blank();
  e.ota_seq = seq;
  e.ota_state = state;
  e.crc = computeSeqCrc(seq);
  return e;
}
bool validFork(const Otadata& data, const BootableSlots& images) {
  for (const auto& e : data) {
    if (e.ota_seq != UINT32_MAX && e.crc == computeSeqCrc(e.ota_seq) && e.ota_state == kOtaImgValid &&
        images.slots[(e.ota_seq - 1) % 2])
      return true;
  }
  return false;
}
int bootSelection(const Otadata& data) {
  int selected = -1;
  for (int i = 0; i < 2; ++i) {
    const auto& e = data[i];
    if (e.ota_seq == UINT32_MAX || e.crc != computeSeqCrc(e.ota_seq) || e.ota_state == kOtaImgPendingVerify ||
        e.ota_state == kOtaImgAborted || e.ota_state == kOtaImgInvalid)
      continue;
    if (selected < 0 || e.ota_seq > data[selected].ota_seq) selected = i;
  }
  return selected;
}
void assertWrite(const Otadata& before, const BootableSlots& images, const EntryWrite& write, uint32_t slot,
                 uint32_t state) {
  ASSERT_LT(write.sector, 2u);
  Otadata after;
  std::memcpy(after, before, sizeof(after));
  after[write.sector] = blank();
  ASSERT_TRUE(validFork(after, images)) << "erase removed the last VALID fork";
  after[write.sector] = write.entry;
  ASSERT_TRUE(validFork(after, images));
  const int selected = bootSelection(after);
  ASSERT_GE(selected, 0);
  EXPECT_EQ((after[selected].ota_seq - 1) % 2, slot);
  EXPECT_EQ(after[selected].ota_state, state);
  EXPECT_EQ(selected, write.sector);
}
}  // namespace

TEST(OtaWriteInvariant, ExhaustiveBothSectorsStatesSlotsSequenceEdgesRunningAndBootableImageSets) {
  std::array<SelectEntry, 84> candidates{};
  size_t n = 0;
  for (uint32_t kind = 0; kind < 7; ++kind) {
    for (uint32_t slot = 0; slot < 2; ++slot) {
      for (uint32_t near : {1u, 2u, 3u, 0xfffffffdu, 0xfffffffeu, 0xffffffffu}) {
        uint32_t seq = near;
        if (near != UINT32_MAX && (seq - 1) % 2 != slot) seq = seq == 1 ? 2 : seq - 1;
        const uint32_t states[] = {kOtaImgValid,         kOtaImgValid,   kOtaImgValid,  kOtaImgNew,
                                   kOtaImgPendingVerify, kOtaImgAborted, kOtaImgInvalid};
        auto e = kind == 0 ? blank() : entry(seq, states[kind]);
        if (kind == 1) e.crc ^= 1;
        candidates[n++] = e;
      }
    }
  }
  size_t writes = 0;
  size_t refused = 0;
  for (const auto& first : candidates) {
    for (const auto& second : candidates) {
      const Otadata data = {first, second};
      for (uint32_t running = 0; running < 2; ++running) {
        // Per slot: absent, corrupt, guest, CP1/shop, CP2.
        for (uint32_t imagePair = 0; imagePair < 25; ++imagePair) {
          const BootableSlots images{{imagePair / 5 >= 3, imagePair % 5 >= 3}};
          for (uint32_t target = 0; target < 2; ++target) {
            auto afterErase = images;
            afterErase.slots[target] = false;
            EXPECT_EQ(appEraseIsSafe(data, images, target), validFork(data, afterErase));
            for (uint32_t state : {kOtaImgNew, kOtaImgValid}) {
              SCOPED_TRACE(::testing::Message() << first.ota_seq << '/' << first.ota_state << ", " << second.ota_seq
                                                << '/' << second.ota_state << " running=" << running << " bootable="
                                                << imagePair << " target=" << target << " state=" << state);
              EntryWrite write{99, blank()};
              if (planSelect(data, images, target, state, write)) {
                ++writes;
                assertWrite(data, images, write, target, state);
              } else {
                ++refused;
                EXPECT_EQ(write.sector, 99u);
              }
            }
          }
          EntryWrite keep{99, blank()};
          switch (planKeepRunningSlot(data, images, running, keep)) {
            case KeepPlan::Write:
              ++writes;
              assertWrite(data, images, keep, running, kOtaImgNew);
              break;
            case KeepPlan::NoWrite: {
              const int selected = bootSelection(data);
              ASSERT_GE(selected, 0);
              EXPECT_EQ((data[selected].ota_seq - 1) % 2, running);
              EXPECT_EQ(keep.sector, 99u);
              break;
            }
            case KeepPlan::Unsafe:
              ++refused;
              EXPECT_EQ(keep.sector, 99u);
              break;
          }
        }
      }
    }
  }
  EXPECT_GT(writes, 0u);
  EXPECT_GT(refused, writes);
}

TEST(OtaWriteInvariant, ArmedNewForkPreservesOlderValidForkInAllEightOrders) {
  for (uint32_t running = 0; running < 2; ++running) {
    for (uint32_t sector = 0; sector < 2; ++sector) {
      for (uint32_t state : {kOtaImgNew, kOtaImgPendingVerify}) {
        Otadata data{};
        data[sector] = entry(3 + running, state);
        data[1 - sector] = entry(2 + running, kOtaImgValid);
        EntryWrite write{};
        const BootableSlots images{{true, true}};
        ASSERT_TRUE(planSelect(data, images, 1 - running, kOtaImgValid, write));
        EXPECT_EQ(write.sector, sector);
        assertWrite(data, images, write, 1 - running, kOtaImgValid);
        data[sector] = blank();
        ASSERT_TRUE(planSelect(data, images, 1 - running, kOtaImgValid, write));
        assertWrite(data, images, write, 1 - running, kOtaImgValid);
      }
    }
  }
}

TEST(OtaWriteInvariant, FellBackKeepAndConfirmationUseExplicitRunningSlot) {
  const Otadata data = {entry(3, kOtaImgValid), entry(4, kOtaImgValid)};
  const BootableSlots images{{true, false}};
  EntryWrite write{};
  ASSERT_EQ(planKeepRunningSlot(data, images, 0, write), KeepPlan::Write);
  EXPECT_EQ(write.sector, 1u);
  assertWrite(data, images, write, 0, kOtaImgNew);
  ASSERT_TRUE(planSelect(data, images, 0, kOtaImgValid, write));
  EXPECT_EQ(write.sector, 1u);
  assertWrite(data, images, write, 0, kOtaImgValid);
}

TEST(OtaWriteInvariant, ExhaustedArmedGuestCannotBeKeptForSleep) {
  const Otadata data = {entry(0xfffffffdu, kOtaImgPendingVerify), entry(0xfffffffeu, kOtaImgValid)};
  EntryWrite write{99, blank()};
  EXPECT_EQ(planKeepRunningSlot(data, BootableSlots{{true, false}}, 0, write), KeepPlan::Unsafe);
  EXPECT_EQ(write.sector, 99u);
}

TEST(OtaWriteInvariant, GuardRejectsEraseOfLastForkEvenWhenFinalWriteRestoresIt) {
  const Otadata data = {entry(3, kOtaImgValid), blank()};
  const EntryWrite write{0, entry(5, kOtaImgValid)};
  EXPECT_FALSE(writeIsSafe(data, BootableSlots{{true, false}}, write, 0, kOtaImgValid));
}

TEST(OtaWriteInvariant, GuardRejectsWrongSlotBadCrcPendingAndLosingSequence) {
  const Otadata data = {entry(3, kOtaImgValid), blank()};
  const BootableSlots images{{true, false}};
  EXPECT_FALSE(writeIsSafe(data, images, EntryWrite{1, entry(2, kOtaImgNew)}, 1, kOtaImgNew));
  EXPECT_FALSE(writeIsSafe(data, images, EntryWrite{1, entry(4, kOtaImgPendingVerify)}, 1, kOtaImgPendingVerify));
  EXPECT_FALSE(writeIsSafe(data, images, EntryWrite{1, entry(4, kOtaImgNew)}, 0, kOtaImgNew));
  auto bad = entry(4, kOtaImgNew);
  bad.crc ^= 1;
  EXPECT_FALSE(writeIsSafe(data, images, EntryWrite{1, bad}, 1, kOtaImgNew));
}

TEST(OtaWriteInvariant, OnlyLegacyPolicyAllowsSelectionFromBlankOtadata) {
  const Otadata data = {blank(), blank()};
  EntryWrite write{};
  EXPECT_FALSE(planSelect(data, BootableSlots{}, 1, kOtaImgNew, write));
  ASSERT_TRUE(planSelect(data, BootableSlots{}, 1, kOtaImgNew, write, WritePolicy::Legacy));
  EXPECT_EQ(write.entry.ota_seq, 2u);
}

TEST(OtaWriteInvariant, RecoveryRefusesErasingOnlyValidImageUntilHomeConfirms) {
  for (uint32_t state : {kOtaImgNew, kOtaImgPendingVerify}) {
    Otadata data = {entry(3, kOtaImgValid), entry(4, state)};
    const BootableSlots images{{true, true}};
    EntryWrite write{};
    EXPECT_FALSE(appEraseIsSafe(data, images, 0));
    ASSERT_TRUE(planSelect(data, images, 1, kOtaImgValid, write));
    assertWrite(data, images, write, 1, kOtaImgValid);
    data[write.sector] = write.entry;
    EXPECT_TRUE(appEraseIsSafe(data, images, 0));
    const BootableSlots afterFirstErase{{false, true}};
    EXPECT_TRUE(validFork(data, afterFirstErase));
    EXPECT_EQ(entrySlot(data[bootSelection(data)]), 1u);
  }
}
