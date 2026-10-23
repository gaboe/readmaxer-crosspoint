#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "GuestSlotModel.h"
#include "OtaSlotModel.h"

using namespace ota_boot;

namespace {

constexpr uint32_t kPendingVerify = 1;  // ESP_OTA_IMG_PENDING_VERIFY
constexpr uint16_t kChipS3 = 9;

using Otadata = SelectEntry[kEntrySectors];

SelectEntry blankEntry() {
  SelectEntry e;
  std::memset(&e, 0xFF, sizeof(e));
  return e;
}

SelectEntry entry(uint32_t seq, uint32_t state) {
  SelectEntry e = blankEntry();
  e.ota_seq = seq;
  e.ota_state = state;
  e.crc = computeSeqCrc(seq);
  return e;
}

void apply(Otadata& otadata, const EntryWrite& write) { otadata[write.sector] = write.entry; }

BootableSlots forkSlots(uint32_t running, bool otherFork = false) {
  BootableSlots images{};
  images.slots[running] = true;
  images.slots[1u - running] = otherFork;
  return images;
}

EntryWrite selectWrite(const Otadata& entries, uint32_t running, uint32_t slot, uint32_t state,
                       bool otherFork = false) {
  EntryWrite write{};
  EXPECT_TRUE(ota_boot::planSelect(entries, forkSlots(running, otherFork), slot, state, write));
  return write;
}

// IDF bootloader slot selection (bootloader_utility_get_selected_boot_partition)
// with no factory partition: a PENDING_VERIFY entry that reaches another boot
// is aborted, a NEW entry becomes PENDING_VERIFY, and blank otadata boots ota_0.
uint32_t bootloaderSelect(Otadata& otadata, bool rollback) {
  if (rollback) {
    for (auto& e : otadata)
      if (e.ota_state == kPendingVerify) e.ota_state = kOtaImgAborted;
  }
  int active = activeEntry(otadata);
  if (active < 0) return 0;
  if (rollback && otadata[active].ota_state == kOtaImgNew) otadata[active].ota_state = kPendingVerify;
  return entrySlot(otadata[active]);
}

// What the fork does in setup(), with the other slot holding an app image.
BootPlan forkSetup(Otadata& otadata, uint32_t runningSlot, bool deepSleepWake = false) {
  const BootPlan plan = planBoot(otadata, runningSlot, deepSleepWake);
  if (plan.armFallback) {
    EntryWrite write{};
    if (ota_boot::planSelect(otadata, forkSlots(runningSlot, true), 1 - runningSlot, kOtaImgValid, write))
      apply(otadata, write);
  }
  return plan;
}

void forkHomeRendered(Otadata& otadata, uint32_t runningSlot, const BootPlan& plan) {
  if (plan.confirmAfterHome) apply(otadata, selectWrite(otadata, runningSlot, runningSlot, kOtaImgValid, true));
}

// keepRunningSlot() before a deliberate sleep or idle while confirmation is pending.
void forkKeepsRunningSlot(Otadata& otadata, uint32_t runningSlot, const BootPlan& plan) {
  EntryWrite write{};
  if (plan.confirmAfterHome &&
      planKeepRunningSlot(otadata, forkSlots(runningSlot, true), runningSlot, write) == KeepPlan::Write)
    apply(otadata, write);
}

class FallbackBothRollbackSettings : public ::testing::TestWithParam<bool> {};

}  // namespace

TEST(OtaSlotModel, SeqCrcMatchesIdfOtatool) {
  // binascii.crc32(struct.pack('<I', seq), 0xFFFFFFFF), as IDF otatool.py computes it.
  EXPECT_EQ(computeSeqCrc(1), 0x4743989Au);
  EXPECT_EQ(computeSeqCrc(2), 0x55F63774u);
  EXPECT_EQ(computeSeqCrc(3), 0xED4A5011u);
  EXPECT_EQ(computeSeqCrc(16), 0xAFE6A860u);
}

TEST(OtaSlotModel, ActiveEntryIgnoresBlankBadCrcAbortedAndInvalid) {
  Otadata blank = {blankEntry(), blankEntry()};
  EXPECT_EQ(activeEntry(blank), -1);

  Otadata badCrc = {entry(1, kOtaImgValid), entry(2, kOtaImgValid)};
  badCrc[1].crc ^= 1;
  EXPECT_EQ(activeEntry(badCrc), 0);

  Otadata aborted = {entry(1, kOtaImgValid), entry(2, kOtaImgAborted)};
  EXPECT_EQ(activeEntry(aborted), 0);

  Otadata invalid = {entry(3, kOtaImgInvalid), entry(2, kOtaImgNew)};
  EXPECT_EQ(activeEntry(invalid), 1);

  Otadata tie = {entry(4, kOtaImgValid), entry(4, kOtaImgValid)};
  EXPECT_EQ(activeEntry(tie), 0);
}

TEST(OtaSlotModel, PlanSelectWritesTheInactiveSectorWithTheNextSeqForTheSlot) {
  Otadata otadata = {entry(3, kOtaImgValid), blankEntry()};  // seq 3 selects slot 0
  const EntryWrite toSlot1 = selectWrite(otadata, 0, 1, kOtaImgNew);
  EXPECT_EQ(toSlot1.sector, 1u);
  EXPECT_EQ(toSlot1.entry.ota_seq, 4u);
  EXPECT_EQ(toSlot1.entry.ota_state, kOtaImgNew);
  EXPECT_EQ(toSlot1.entry.crc, computeSeqCrc(4));
  for (const uint8_t b : toSlot1.entry.seq_label) EXPECT_EQ(b, 0xFF);

  const EntryWrite toSlot0 = selectWrite(otadata, 0, 0, kOtaImgValid);
  EXPECT_EQ(toSlot0.sector, 1u);
  EXPECT_EQ(toSlot0.entry.ota_seq, 5u);

  Otadata blank = {blankEntry(), blankEntry()};
  EntryWrite fromBlank{};
  EXPECT_FALSE(ota_boot::planSelect(blank, forkSlots(0, true), 1, kOtaImgValid, fromBlank));
}

TEST(OtaSlotModel, GuestSwitchIsNewAndPreservesForkSectorOnTornWrite) {
  for (uint32_t forkSlot = 0; forkSlot < kSlotCount; ++forkSlot) {
    for (uint32_t forkSector = 0; forkSector < kEntrySectors; ++forkSector) {
      Otadata data = {blankEntry(), blankEntry()};
      data[forkSector] = entry(forkSlot + 1, kOtaImgValid);
      const auto forkEntry = data[forkSector];
      const EntryWrite guestWrite = selectWrite(data, forkSlot, 1 - forkSlot, kOtaImgNew);
      EXPECT_EQ(guestWrite.sector, 1 - forkSector);
      EXPECT_EQ(guestWrite.entry.ota_state, kOtaImgNew);
      EXPECT_EQ(entrySlot(guestWrite.entry), 1 - forkSlot);
      EXPECT_EQ(std::memcmp(&data[forkSector], &forkEntry, sizeof(forkEntry)), 0);
      apply(data, guestWrite);
      EXPECT_EQ(bootloaderSelect(data, true), 1 - forkSlot);
      EXPECT_EQ(bootloaderSelect(data, true), forkSlot);
    }
  }
}

TEST(OtaSlotModel, ArmedFallbackNeverPermitsGuestSwitchToEraseForkSector) {
  for (uint32_t forkSlot = 0; forkSlot < kSlotCount; ++forkSlot) {
    for (uint32_t forkSector = 0; forkSector < kEntrySectors; ++forkSector) {
      Otadata data = {blankEntry(), blankEntry()};
      data[forkSector] = entry(forkSlot + 1, kOtaImgValid);
      const EntryWrite armed = selectWrite(data, forkSlot, 1 - forkSlot, kOtaImgValid);
      apply(data, armed);
      ASSERT_EQ(checkRunningSlot(data, forkSlot), SlotCheck::FellBack);
      EXPECT_FALSE(guest_slot::canSwitch(checkRunningSlot(data, forkSlot), true));
      EXPECT_FALSE(guest_slot::canSwitch(checkRunningSlot(data, forkSlot), false));
      EntryWrite safe{};
      ASSERT_TRUE(ota_boot::planSelect(data, forkSlots(forkSlot), 1 - forkSlot, kOtaImgNew, safe));
      EXPECT_NE(safe.sector, forkSector);
      EXPECT_EQ(data[forkSector].ota_state, kOtaImgValid);
    }
  }
}

TEST(OtaSlotModel, EveryWritePlanPreservesNewestValidRunningEntry) {
  for (uint32_t forkSlot = 0; forkSlot < kSlotCount; ++forkSlot) {
    for (uint32_t forkSector = 0; forkSector < kEntrySectors; ++forkSector) {
      Otadata data = {blankEntry(), blankEntry()};
      data[forkSector] = entry(forkSlot + 3u, kOtaImgValid);
      data[1u - forkSector] = entry(forkSlot + 4u, kOtaImgValid);
      const auto protectedEntry = data[forkSector];
      EntryWrite write{};
      ASSERT_EQ(planKeepRunningSlot(data, forkSlots(forkSlot), forkSlot, write), KeepPlan::Write);
      EXPECT_EQ(write.sector, 1u - forkSector);
      apply(data, write);
      EXPECT_EQ(std::memcmp(&data[forkSector], &protectedEntry, sizeof(protectedEntry)), 0);
      for (uint32_t target = 0; target < kSlotCount; ++target) {
        ASSERT_TRUE(ota_boot::planSelect(data, forkSlots(forkSlot), target, kOtaImgNew, write));
        EXPECT_EQ(write.sector, 1u - forkSector);
      }
    }
  }
}

TEST(OtaSlotModel, SelectRefusesBlankAndWrappedSequences) {
  for (uint32_t sector = 0; sector < kEntrySectors; ++sector) {
    for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
      Otadata data = {blankEntry(), blankEntry()};
      data[sector] = entry(0xFFFFFFFEu, kOtaImgValid);
      EntryWrite write{};
      EXPECT_FALSE(ota_boot::planSelect(data, forkSlots(slot), 0, kOtaImgNew, write));
      EXPECT_FALSE(ota_boot::planSelect(data, forkSlots(slot), 1, kOtaImgNew, write));
      EXPECT_EQ(planKeepRunningSlot(data, forkSlots(slot), 1u - entrySlot(data[sector]), write), KeepPlan::Unsafe);
    }
  }
  Otadata lastUsable = {entry(0xFFFFFFFDu, kOtaImgValid), blankEntry()};
  EntryWrite write{};
  ASSERT_TRUE(ota_boot::planSelect(lastUsable, forkSlots(0), 1, kOtaImgNew, write));
  EXPECT_EQ(write.entry.ota_seq, 0xFFFFFFFEu);
  apply(lastUsable, write);
  EXPECT_EQ(activeEntry(lastUsable), 1);
}

TEST(OtaSlotModel, CheckRunningSlotClassifiesEveryOtadataState) {
  Otadata blank = {blankEntry(), blankEntry()};
  EXPECT_EQ(checkRunningSlot(blank, 0), SlotCheck::Unconfirmed);

  Otadata ownNew = {entry(1, kOtaImgValid), entry(2, kOtaImgNew)};
  EXPECT_EQ(checkRunningSlot(ownNew, 1), SlotCheck::Unconfirmed);

  Otadata ownPending = {entry(1, kOtaImgValid), entry(2, kPendingVerify)};
  EXPECT_EQ(checkRunningSlot(ownPending, 1), SlotCheck::Unconfirmed);

  Otadata ownUndefined = {entry(1, kOtaImgValid), entry(2, 0xFFFFFFFFu)};
  EXPECT_EQ(checkRunningSlot(ownUndefined, 1), SlotCheck::Unconfirmed);

  Otadata ownValid = {entry(1, kOtaImgValid), entry(2, kOtaImgValid)};
  EXPECT_EQ(checkRunningSlot(ownValid, 1), SlotCheck::Confirmed);

  // Running slot 0 while the active entry selects slot 1: the bootloader fell back.
  EXPECT_EQ(checkRunningSlot(ownValid, 0), SlotCheck::FellBack);
}

TEST(OtaSlotModel, PlanBootArmsOnlyAnUnconfirmedColdBoot) {
  Otadata ownNew = {entry(1, kOtaImgValid), entry(2, kOtaImgNew)};
  EXPECT_TRUE(planBoot(ownNew, 1, false).armFallback);
  EXPECT_TRUE(planBoot(ownNew, 1, false).confirmAfterHome);

  const BootPlan deepSleep = planBoot(ownNew, 1, true);
  EXPECT_FALSE(deepSleep.armFallback);
  EXPECT_TRUE(deepSleep.confirmAfterHome);

  Otadata ownValid = {entry(1, kOtaImgValid), entry(2, kOtaImgValid)};
  const BootPlan confirmed = planBoot(ownValid, 1, false);
  EXPECT_FALSE(confirmed.armFallback);
  EXPECT_FALSE(confirmed.confirmAfterHome);

  const BootPlan fellBack = planBoot(ownValid, 0, false);
  EXPECT_FALSE(fellBack.armFallback);
  EXPECT_TRUE(fellBack.confirmAfterHome);

  Otadata blank = {blankEntry(), blankEntry()};
  EXPECT_TRUE(planBoot(blank, 0, false).armFallback);
}

TEST(OtaSlotModel, KeepRunningSlotReselectsTheRunningSlotAsNew) {
  // Armed: the active entry selects the other slot.
  Otadata armed = {entry(3, kOtaImgValid), entry(2, kPendingVerify)};
  EntryWrite write{};
  ASSERT_EQ(planKeepRunningSlot(armed, forkSlots(1, true), 1, write), KeepPlan::Write);
  EXPECT_EQ(write.sector, 1u);
  EXPECT_EQ(write.entry.ota_seq, 4u);
  EXPECT_EQ(write.entry.ota_state, kOtaImgNew);

  // Pending verify under rollback: rewritten in place, the other sector keeps its entry.
  Otadata pending = {entry(3, kOtaImgValid), entry(4, kPendingVerify)};
  ASSERT_EQ(planKeepRunningSlot(pending, forkSlots(1, true), 1, write), KeepPlan::Write);
  EXPECT_EQ(write.sector, 1u);
  EXPECT_EQ(write.entry.ota_seq, 4u);
  EXPECT_EQ(write.entry.ota_state, kOtaImgNew);
  EXPECT_EQ(write.entry.crc, computeSeqCrc(4));

  Otadata ownNew = {entry(3, kOtaImgValid), entry(4, kOtaImgNew)};
  EXPECT_EQ(planKeepRunningSlot(ownNew, forkSlots(1, true), 1, write), KeepPlan::NoWrite);
  Otadata ownValid = {entry(3, kOtaImgValid), entry(4, kOtaImgValid)};
  EXPECT_EQ(planKeepRunningSlot(ownValid, forkSlots(1, true), 1, write), KeepPlan::NoWrite);
}

TEST_P(FallbackBothRollbackSettings, CrashBeforeHomeBootsTheOtherSlotOnEveryLaterReset) {
  const bool rollback = GetParam();
  // Host in slot 0 is VALID; an install wrote the fork into slot 1 as NEW.
  Otadata otadata = {entry(1, kOtaImgValid), entry(2, kOtaImgNew)};
  ASSERT_EQ(bootloaderSelect(otadata, rollback), 1u);
  const BootPlan plan = forkSetup(otadata, 1);
  EXPECT_TRUE(plan.armFallback);

  // The fork crashes before Home renders.
  EXPECT_EQ(bootloaderSelect(otadata, rollback), 0u);
  // The other image never touches otadata and resets again.
  EXPECT_EQ(bootloaderSelect(otadata, rollback), 0u);
}

TEST_P(FallbackBothRollbackSettings, HomeRenderKeepsTheForkSelected) {
  const bool rollback = GetParam();
  Otadata otadata = {entry(1, kOtaImgValid), entry(2, kOtaImgNew)};
  ASSERT_EQ(bootloaderSelect(otadata, rollback), 1u);
  const BootPlan plan = forkSetup(otadata, 1);
  forkHomeRendered(otadata, 1, plan);
  EXPECT_EQ(checkRunningSlot(otadata, 1), SlotCheck::Confirmed);

  for (int boot = 0; boot < 3; ++boot) {
    ASSERT_EQ(bootloaderSelect(otadata, rollback), 1u);
    const BootPlan next = forkSetup(otadata, 1);
    EXPECT_FALSE(next.armFallback);
    EXPECT_FALSE(next.confirmAfterHome);
  }
}

TEST_P(FallbackBothRollbackSettings, BlankOtadataRefusesWritesWithoutAValidFork) {
  const bool rollback = GetParam();
  Otadata otadata = {blankEntry(), blankEntry()};
  ASSERT_EQ(bootloaderSelect(otadata, rollback), 0u);
  const BootPlan plan = forkSetup(otadata, 0);
  ASSERT_TRUE(plan.armFallback);
  EntryWrite write{};
  EXPECT_FALSE(ota_boot::planSelect(otadata, forkSlots(0, true), 0, kOtaImgValid, write));
  EXPECT_EQ(bootloaderSelect(otadata, rollback), 0u);
  EXPECT_EQ(checkRunningSlot(otadata, 0), SlotCheck::Unconfirmed);
}

TEST_P(FallbackBothRollbackSettings, SleepBeforeHomeWakesTheForkAndAPreHomeCrashStillFallsBack) {
  const bool rollback = GetParam();
  Otadata otadata = {entry(1, kOtaImgValid), entry(2, kOtaImgNew)};
  ASSERT_EQ(bootloaderSelect(otadata, rollback), 1u);
  BootPlan plan = forkSetup(otadata, 1);
  ASSERT_TRUE(plan.armFallback);
  forkKeepsRunningSlot(otadata, 1, plan);  // ghost wake: back to sleep before Home

  // Wake without fast wake: the bootloader reads otadata again. Sleep twice more.
  for (int wake = 0; wake < 2; ++wake) {
    ASSERT_EQ(bootloaderSelect(otadata, rollback), 1u);
    plan = forkSetup(otadata, 1, /*deepSleepWake=*/true);
    EXPECT_FALSE(plan.armFallback);
    forkKeepsRunningSlot(otadata, 1, plan);
  }

  // The fork crashes before Home on the next wake and on every later boot.
  ASSERT_EQ(bootloaderSelect(otadata, rollback), 1u);
  forkSetup(otadata, 1, /*deepSleepWake=*/true);
  uint32_t booted = bootloaderSelect(otadata, rollback);
  if (booted == 1u) {
    // Rollback off: the panic reset is a cold boot, so it arms again.
    forkSetup(otadata, 1);
    booted = bootloaderSelect(otadata, rollback);
  }
  EXPECT_EQ(booted, 0u);
  EXPECT_EQ(bootloaderSelect(otadata, rollback), 0u);
}

TEST_P(FallbackBothRollbackSettings, SleepBeforeHomeThenHomeConfirms) {
  const bool rollback = GetParam();
  Otadata otadata = {entry(1, kOtaImgValid), entry(2, kOtaImgNew)};
  ASSERT_EQ(bootloaderSelect(otadata, rollback), 1u);
  BootPlan plan = forkSetup(otadata, 1);
  forkKeepsRunningSlot(otadata, 1, plan);
  ASSERT_EQ(bootloaderSelect(otadata, rollback), 1u);
  plan = forkSetup(otadata, 1, /*deepSleepWake=*/true);
  forkHomeRendered(otadata, 1, plan);
  EXPECT_EQ(checkRunningSlot(otadata, 1), SlotCheck::Confirmed);
  EXPECT_EQ(bootloaderSelect(otadata, rollback), 1u);
}

INSTANTIATE_TEST_SUITE_P(Rollback, FallbackBothRollbackSettings, ::testing::Bool());

TEST(OtaSlotModel, AbortedNewFallbackRetainsTheOlderValidFork) {
  // Why the fallback writes VALID: a NEW entry for an image that never marks
  // itself valid is aborted on its second boot, and the crashing fork returns.
  Otadata otadata = {entry(1, kOtaImgValid), entry(2, kOtaImgNew)};
  ASSERT_EQ(bootloaderSelect(otadata, true), 1u);
  apply(otadata, selectWrite(otadata, 1, 0, kOtaImgNew, true));
  EXPECT_EQ(bootloaderSelect(otadata, true), 0u);
  EXPECT_EQ(bootloaderSelect(otadata, true), 0u);
}

namespace {

// Plain SHA-256 (FIPS 180-4), so the image tests check real digests.
class Sha256 {
 public:
  Sha256() { reset(); }
  void reset() {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, init, sizeof(h_));
    len_ = 0;
    used_ = 0;
  }
  void update(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      block_[used_++] = data[i];
      if (used_ == 64) {
        compress();
        used_ = 0;
      }
    }
    len_ += len;
  }
  void finish(uint8_t (&digest)[32]) {
    const uint64_t bits = len_ * 8;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0;
    while (used_ != 56) update(&zero, 1);
    for (int i = 7; i >= 0; --i) {
      const auto b = static_cast<uint8_t>(bits >> (i * 8));
      update(&b, 1);
    }
    for (int i = 0; i < 8; ++i)
      for (int j = 0; j < 4; ++j) digest[i * 4 + j] = static_cast<uint8_t>(h_[i] >> (24 - j * 8));
  }

 private:
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void compress() {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t{block_[i * 4]} << 24) | (uint32_t{block_[i * 4 + 1]} << 16) |
             (uint32_t{block_[i * 4 + 2]} << 8) | block_[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
  }

  uint32_t h_[8];
  uint8_t block_[64];
  uint64_t len_;
  size_t used_;
};

std::vector<uint8_t> appImage(uint16_t chip, const std::vector<uint32_t>& segmentLens, bool shaAppended,
                              uint32_t firstAddress = 0x3C000020) {
  std::vector<uint8_t> image(24, 0);
  image[0] = 0xE9;
  image[1] = static_cast<uint8_t>(segmentLens.size() + 1);
  std::memcpy(&image[12], &chip, sizeof(chip));
  image[23] = shaAppended ? 1 : 0;
  uint8_t checksum = 0xEF;
  uint32_t address = firstAddress;
  for (const uint32_t len : segmentLens) {
    uint8_t seg[8] = {};
    std::memcpy(seg, &address, sizeof(address));
    std::memcpy(seg + 4, &len, sizeof(len));
    image.insert(image.end(), seg, seg + 8);
    for (uint32_t i = 0; i < len; ++i) {
      uint8_t b = static_cast<uint8_t>(i * 7 + len);
      if (image.size() - i == 32 && i < 256) {
        b = 0;
        if (i < 4) b = static_cast<uint8_t>(0xABCD5432u >> (i * 8));
      }
      image.push_back(b);
      checksum ^= b;
    }
    address = 0x40374000;
  }
  const uint32_t mappedAddress = 0x42000000u + (image.size() + 8) % 65536;
  const uint32_t mappedLength = 4;
  const auto* addressBytes = reinterpret_cast<const uint8_t*>(&mappedAddress);
  const auto* lengthBytes = reinterpret_cast<const uint8_t*>(&mappedLength);
  image.insert(image.end(), addressBytes, addressBytes + 4);
  image.insert(image.end(), lengthBytes, lengthBytes + 4);
  image.insert(image.end(), 4, 0);
  image.resize((image.size() + 16) & ~static_cast<size_t>(15), 0);
  image.back() = checksum;
  if (shaAppended) {
    Sha256 sha;
    sha.update(image.data(), image.size());
    uint8_t digest[32];
    sha.finish(digest);
    image.insert(image.end(), digest, digest + 32);
  }
  return image;
}

bool readVector(uint32_t offset, void* dst, uint32_t len, void* ctx) {
  const auto* image = static_cast<const std::vector<uint8_t>*>(ctx);
  if (offset > image->size() || len > image->size() - offset) return false;
  std::memcpy(dst, image->data() + offset, len);
  return true;
}

bool holds(std::vector<uint8_t> slot, uint32_t slotSize, uint16_t chip = kChipS3,
           guest_slot::Scanner* scanner = nullptr) {
  Sha256 sha;
  const ImageHasher hasher{&sha,
                           [](void* ctx, const uint8_t* data, uint32_t len) {
                             static_cast<Sha256*>(ctx)->update(data, len);
                             return true;
                           },
                           [](void* ctx, uint8_t (&digest)[32]) {
                             static_cast<Sha256*>(ctx)->finish(digest);
                             return true;
                           }};
  uint8_t buf[64];  // smaller than a segment, so the chunk loop runs
  slot.resize(std::max<size_t>(slot.size(), slotSize), 0xFF);
  const auto visit = [](const uint8_t* data, uint32_t len, void* ctx) {
    static_cast<guest_slot::Scanner*>(ctx)->feed(data, len);
  };
  return checkAppImage(readVector, &slot, slotSize, chip, hasher, buf, sizeof(buf), scanner ? visit : nullptr,
                       scanner) == ImageCheck::Ok;
}

}  // namespace

TEST(OtaSlotModel, Sha256MatchesTheFipsVector) {
  Sha256 sha;
  const uint8_t abc[] = {'a', 'b', 'c'};
  sha.update(abc, sizeof(abc));
  uint8_t digest[32];
  sha.finish(digest);
  const uint8_t expected[4] = {0xba, 0x78, 0x16, 0xbf};
  EXPECT_EQ(std::memcmp(digest, expected, sizeof(expected)), 0);
  EXPECT_EQ(digest[31], 0xad);
}

TEST(OtaSlotModel, SlotImageCheckAcceptsAWellFormedImage) {
  const auto image = appImage(kChipS3, {256, 4096, 8}, true);
  EXPECT_TRUE(holds(image, static_cast<uint32_t>(image.size())));
  EXPECT_TRUE(holds(image, 0x10000));  // erased flash after the image
  EXPECT_TRUE(holds(appImage(kChipS3, {256}, true), 0x1000));
  EXPECT_FALSE(holds(appImage(kChipS3, {256}, true, 0x42000020), 0x1000));
}

TEST(OtaSlotModel, MappedSegmentMustMatchFlashMmuOffset) {
  const auto aligned = appImage(kChipS3, {256}, true, 0x3C000020);
  const auto misaligned = appImage(kChipS3, {256}, true, 0x3C000024);
  EXPECT_TRUE(holds(aligned, static_cast<uint32_t>(aligned.size())));
  EXPECT_FALSE(holds(misaligned, static_cast<uint32_t>(misaligned.size())));

  SelectEntry entries[2] = {entry(3, kOtaImgValid), entry(5, kPendingVerify)};
  const BootableSlots sound{{true, false}};
  EntryWrite fallback{};
  ASSERT_EQ(planKeepRunningSlot(entries, sound, 0, fallback), KeepPlan::Write);
  EXPECT_EQ(fallback.sector, 1u);
  SelectEntry afterErase[2] = {entries[0], entries[1]};
  std::memset(&afterErase[fallback.sector], 0xFF, sizeof(SelectEntry));
  EXPECT_EQ(afterErase[0].ota_state, kOtaImgValid);
  EXPECT_TRUE(appEraseIsSafe(afterErase, sound, 1));
}

TEST(OtaSlotModel, AuthenticatedGuestImageNeedsProjectAndOneMarker) {
  auto image = appImage(kChipS3, {256}, true);
  const uint32_t magic = 0xABCD5432;
  std::memcpy(image.data() + 32, &magic, sizeof(magic));
  std::memcpy(image.data() + 48, "1.2.3", 6);
  std::memcpy(image.data() + 80, "readmaxer-xteink-x4pro", sizeof("readmaxer-xteink-x4pro"));
  std::memcpy(image.data() + 240, "READMAXER-GUEST-V1;", sizeof("READMAXER-GUEST-V1;") - 1);
  uint8_t checksum = 0xEF;
  for (size_t i = 32; i < 288; ++i) checksum ^= image[i];
  image[image.size() - 33] = checksum;
  Sha256 sha;
  sha.update(image.data(), image.size() - 32);
  uint8_t digest[32];
  sha.finish(digest);
  std::memcpy(image.data() + image.size() - 32, digest, sizeof(digest));
  guest_slot::Scanner scanner;
  ASSERT_TRUE(holds(image, 4096, kChipS3, &scanner));
  const auto guest = guest_slot::inspectHeader(readVector, &image, image.size());
  EXPECT_TRUE(guest.installed);
  EXPECT_STREQ(guest.version, "1.2.3");
  EXPECT_TRUE(scanner.exactlyOne());
  image[240] ^= 1;
  EXPECT_FALSE(holds(image, 4096));
}

TEST(OtaSlotModel, SlotImageCheckRejectsCorruptChecksumHashAndFirstSegment) {
  const auto image = appImage(kChipS3, {256, 4096, 8}, true);
  const auto slotSize = static_cast<uint32_t>(image.size());

  auto segmentByte = image;
  segmentByte[24 + 8 + 50] ^= 0x01;  // also flips the checksum: caught there first
  EXPECT_FALSE(holds(segmentByte, slotSize));

  auto twoBytes = image;  // checksum still matches, only the hash catches it
  twoBytes[24 + 8 + 50] ^= 0x01;
  twoBytes[24 + 8 + 51] ^= 0x01;
  EXPECT_FALSE(holds(twoBytes, slotSize));

  auto checksumByte = image;
  checksumByte[image.size() - 33] ^= 0x01;
  EXPECT_FALSE(holds(checksumByte, slotSize));

  auto trailer = image;
  trailer[image.size() - 1] ^= 0x01;
  EXPECT_FALSE(holds(trailer, slotSize));

  auto header = image;
  header[2] ^= 0x01;  // flash mode byte: covered only by the hash
  EXPECT_FALSE(holds(header, slotSize));

  EXPECT_FALSE(holds(appImage(kChipS3, {256}, false), 0x1000));             // no SHA-256 trailer
  EXPECT_FALSE(holds(appImage(kChipS3, {62}, true), 0x1000));               // unaligned segment length
  EXPECT_FALSE(holds(appImage(kChipS3, {256}, true, 0x3FC9E100), 0x1000));  // DRAM first
  EXPECT_FALSE(holds(appImage(kChipS3, {256}, true, 0x40374000), 0x1000));  // IRAM first
}
TEST(OtaSlotModel, SlotImageCheckRejectsErasedWrongChipAndTruncatedSlots) {
  EXPECT_FALSE(holds(std::vector<uint8_t>(4096, 0xFF), 4096));
  EXPECT_FALSE(holds(appImage(5, {256}, true), 4096));

  auto noSegments = appImage(kChipS3, {256}, true);
  noSegments[1] = 0;
  EXPECT_FALSE(holds(noSegments, 4096));

  auto tooManySegments = appImage(kChipS3, {256}, true);
  tooManySegments[1] = 17;
  EXPECT_FALSE(holds(tooManySegments, 4096));

  const auto image = appImage(kChipS3, {256, 4096}, true);
  EXPECT_FALSE(holds(image, static_cast<uint32_t>(image.size() - 1)));

  // A segment length read from erased flash runs past the slot.
  auto erasedTail = appImage(kChipS3, {256, 4096}, true);
  std::memset(&erasedTail[24 + 8 + 256], 0xFF, erasedTail.size() - (24 + 8 + 256));
  EXPECT_FALSE(holds(erasedTail, static_cast<uint32_t>(erasedTail.size())));
}

TEST(OtaSlotModel, BootloaderImageRules) {
  const auto good = appImage(kChipS3, {256, 64}, true);
  struct Rule {
    const char* name;
    size_t offset;
    uint32_t value;
    size_t width;
    bool accepted;
  };
  const Rule rules[] = {
      {"magic", 0, 0, 1, false},
      {"chip", 12, 5, 2, false},
      {"minimum revision zero", 15, 0, 2, true},
      {"minimum revision one", 15, 1, 2, false},
      {"maximum revision inclusive", 17, 99, 2, true},
      {"maximum revision too low", 17, 98, 2, false},
      {"maximum revision unset", 17, 65535, 2, true},
      {"legacy min revision ignored by IDF", 14, 255, 1, true},
      {"no segments", 1, 0, 1, false},
      {"missing DROM", 24, 0x42000020, 4, false},
      {"missing IROM", 360, 0x40374000, 4, false},
      {"empty mapped segment", 364, 0, 4, false},
      {"too many segments", 1, 17, 1, false},
      {"descriptor magic", 32, 0, 4, false},
      {"descriptor truncated", 28, 252, 4, false},
      {"block minimum zero", 208, 0, 2, true},
      {"block minimum unset", 208, 65535, 2, true},
      {"block minimum too high", 208, 1, 2, false},
      {"block maximum inclusive", 210, 199, 2, true},
      {"block maximum too low", 210, 198, 2, false},
      {"block maximum unset", 210, 65535, 2, true},
      {"legacy page size", 212, 0, 1, true},
      {"64 KiB page size", 212, 16, 1, true},
      {"32 KiB page size", 212, 15, 1, false},
      {"page shift overflow", 212, 32, 1, false},
      {"mapped aligned", 24, 0x3C000020, 4, true},
      {"mapped misaligned", 24, 0x3C000024, 4, false},
      {"mapped upper range", 288, 0x43FF0128, 4, true},
      {"dram", 288, 0x3FC88000, 4, true},
      {"dram end", 288, 0x3FCDB6C0, 4, true},
      {"dram overlaps loader alias", 288, 0x3FCDB6C4, 4, false},
      {"loader dram alias", 288, 0x3FCDB700, 4, false},
      {"bootloader data", 288, 0x3FCE2700, 4, false},
      {"bootloader stack", 288, 0x3FCE9700, 4, false},
      {"iram", 288, 0x40370000, 4, true},
      {"iram end", 288, 0x403CB6C0, 4, true},
      {"iram overlaps loader", 288, 0x403CB6C4, 4, false},
      {"loader iram", 288, 0x403CB700, 4, false},
      {"static data iram alias", 288, 0x403D2700, 4, false},
      {"rtc fast", 288, 0x600FE000, 4, true},
      {"rtc fast end", 288, 0x600FFFC0, 4, true},
      {"rtc fast overflow", 288, 0x600FFFC4, 4, false},
      {"rtc slow", 288, 0x50000000, 4, true},
      {"rtc slow end", 288, 0x50001FC0, 4, true},
      {"rtc slow overflow", 288, 0x50001FC4, 4, false},
      {"padding", 288, 0, 4, true},
      {"reserved non-load", 288, 4, 4, true},
      {"peripheral", 288, 0x60000000, 4, false},
      {"outside RAM", 288, 0x3FC87FFC, 4, false},
      {"unaligned RAM", 288, 0x3FC88001, 4, false},
      {"address overflow", 288, 0xFFFFFFFC, 4, false},
      {"zero length", 292, 0, 4, false},  // Changing length without moving the checksum is invalid.
      {"unaligned length", 292, 63, 4, false},
      {"16 MiB length", 292, 0x1000000, 4, false},
      {"length beyond slot", 292, 4096, 4, false},
      {"missing SHA", 23, 0, 1, false},
  };
  ASSERT_TRUE(holds(good, good.size()));
  for (const auto& rule : rules) {
    SCOPED_TRACE(rule.name);
    auto image = good;
    std::memcpy(image.data() + rule.offset, &rule.value, rule.width);
    uint8_t checksum = 0xEF;
    for (size_t i = 32; i < 288; ++i) checksum ^= image[i];
    for (size_t i = 296; i < 360; ++i) checksum ^= image[i];
    image[image.size() - 33] = checksum;
    Sha256 sha;
    sha.update(image.data(), image.size() - 32);
    uint8_t digest[32];
    sha.finish(digest);
    std::memcpy(image.data() + image.size() - 32, digest, 32);
    EXPECT_EQ(holds(image, image.size()), rule.accepted);
  }
  EXPECT_TRUE(holds(appImage(kChipS3, {256, 0}, true), 4096));
  EXPECT_TRUE(holds(appImage(kChipS3, std::vector<uint32_t>(15, 256), true), 8192));
  for (const uint32_t address : {0x43FE0128u, 0x43FF0128u}) {
    auto image = appImage(kChipS3, {256, 65536}, true);
    std::memcpy(image.data() + 288, &address, 4);
    Sha256 sha;
    sha.update(image.data(), image.size() - 32);
    uint8_t digest[32];
    sha.finish(digest);
    std::memcpy(image.data() + image.size() - 32, digest, 32);
    EXPECT_EQ(holds(image, image.size()), address == 0x43FE0128u);
  }
  EXPECT_FALSE(holds(good, 31));
  EXPECT_FALSE(holds(good, 290));
  EXPECT_FALSE(holds(good, good.size() - 32));
}
