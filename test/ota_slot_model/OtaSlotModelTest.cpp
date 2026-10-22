#include <gtest/gtest.h>

#include <cstring>
#include <vector>

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

// IDF bootloader slot selection (bootloader_utility_get_selected_boot_partition)
// with no factory partition: a PENDING_VERIFY entry that reaches another boot
// is aborted, a NEW entry becomes PENDING_VERIFY, and blank otadata boots ota_0.
uint32_t bootloaderSelect(Otadata& otadata, bool rollback) {
  int active = activeEntry(otadata);
  if (rollback && active >= 0 && otadata[active].ota_state == kPendingVerify) {
    otadata[active].ota_state = kOtaImgAborted;
    active = activeEntry(otadata);
  }
  if (active < 0) return 0;
  if (rollback && otadata[active].ota_state == kOtaImgNew) otadata[active].ota_state = kPendingVerify;
  return entrySlot(otadata[active]);
}

// What the fork does in setup(), with the other slot holding an app image.
BootPlan forkSetup(Otadata& otadata, uint32_t runningSlot) {
  const BootPlan plan = planBoot(otadata, runningSlot, /*deepSleepWake=*/false, /*otherSlotHoldsImage=*/true);
  if (plan.selectOtherSlot) apply(otadata, planSelect(otadata, 1 - runningSlot, kOtaImgValid));
  return plan;
}

void forkHomeRendered(Otadata& otadata, uint32_t runningSlot, const BootPlan& plan) {
  if (plan.confirmAfterHome) apply(otadata, planSelect(otadata, runningSlot, kOtaImgValid));
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
  const EntryWrite toSlot1 = planSelect(otadata, 1, kOtaImgNew);
  EXPECT_EQ(toSlot1.sector, 1u);
  EXPECT_EQ(toSlot1.entry.ota_seq, 4u);
  EXPECT_EQ(toSlot1.entry.ota_state, kOtaImgNew);
  EXPECT_EQ(toSlot1.entry.crc, computeSeqCrc(4));
  for (const uint8_t b : toSlot1.entry.seq_label) EXPECT_EQ(b, 0xFF);

  const EntryWrite toSlot0 = planSelect(otadata, 0, kOtaImgValid);
  EXPECT_EQ(toSlot0.sector, 1u);
  EXPECT_EQ(toSlot0.entry.ota_seq, 5u);

  Otadata blank = {blankEntry(), blankEntry()};
  const EntryWrite fromBlank = planSelect(blank, 1, kOtaImgValid);
  EXPECT_EQ(fromBlank.sector, 0u);
  EXPECT_EQ(fromBlank.entry.ota_seq, 2u);
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

TEST(OtaSlotModel, PlanBootArmsOnlyAnUnconfirmedColdBootWithAnImageInTheOtherSlot) {
  Otadata ownNew = {entry(1, kOtaImgValid), entry(2, kOtaImgNew)};
  EXPECT_TRUE(planBoot(ownNew, 1, false, true).selectOtherSlot);
  EXPECT_TRUE(planBoot(ownNew, 1, false, true).confirmAfterHome);

  const BootPlan deepSleep = planBoot(ownNew, 1, true, true);
  EXPECT_FALSE(deepSleep.selectOtherSlot);
  EXPECT_TRUE(deepSleep.confirmAfterHome);

  const BootPlan noImage = planBoot(ownNew, 1, false, false);
  EXPECT_FALSE(noImage.selectOtherSlot);
  EXPECT_TRUE(noImage.confirmAfterHome);

  Otadata ownValid = {entry(1, kOtaImgValid), entry(2, kOtaImgValid)};
  const BootPlan confirmed = planBoot(ownValid, 1, false, true);
  EXPECT_FALSE(confirmed.selectOtherSlot);
  EXPECT_FALSE(confirmed.confirmAfterHome);

  const BootPlan fellBack = planBoot(ownValid, 0, false, true);
  EXPECT_FALSE(fellBack.selectOtherSlot);
  EXPECT_TRUE(fellBack.confirmAfterHome);

  Otadata blank = {blankEntry(), blankEntry()};
  EXPECT_TRUE(planBoot(blank, 0, false, true).selectOtherSlot);
}

TEST_P(FallbackBothRollbackSettings, CrashBeforeHomeBootsTheOtherSlotOnEveryLaterReset) {
  const bool rollback = GetParam();
  // Host in slot 0 is VALID; an install wrote the fork into slot 1 as NEW.
  Otadata otadata = {entry(1, kOtaImgValid), entry(2, kOtaImgNew)};
  ASSERT_EQ(bootloaderSelect(otadata, rollback), 1u);
  const BootPlan plan = forkSetup(otadata, 1);
  EXPECT_TRUE(plan.selectOtherSlot);

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
    EXPECT_FALSE(next.selectOtherSlot);
    EXPECT_FALSE(next.confirmAfterHome);
  }
}

TEST_P(FallbackBothRollbackSettings, BlankOtadataAfterUsbFlashArmsAndConfirms) {
  const bool rollback = GetParam();
  Otadata otadata = {blankEntry(), blankEntry()};
  ASSERT_EQ(bootloaderSelect(otadata, rollback), 0u);
  const BootPlan plan = forkSetup(otadata, 0);
  ASSERT_TRUE(plan.selectOtherSlot);
  forkHomeRendered(otadata, 0, plan);
  EXPECT_EQ(bootloaderSelect(otadata, rollback), 0u);
  EXPECT_EQ(checkRunningSlot(otadata, 0), SlotCheck::Confirmed);
}

INSTANTIATE_TEST_SUITE_P(Rollback, FallbackBothRollbackSettings, ::testing::Bool());

TEST(OtaSlotModel, NewFallbackEntryIsAbortedWhenRollbackIsOn) {
  // Why the fallback writes VALID: a NEW entry for an image that never marks
  // itself valid is aborted on its second boot, and the crashing fork returns.
  Otadata otadata = {entry(1, kOtaImgValid), entry(2, kOtaImgNew)};
  ASSERT_EQ(bootloaderSelect(otadata, true), 1u);
  apply(otadata, planSelect(otadata, 0, kOtaImgNew));
  EXPECT_EQ(bootloaderSelect(otadata, true), 0u);
  EXPECT_EQ(bootloaderSelect(otadata, true), 1u);
}

namespace {

std::vector<uint8_t> appImage(uint16_t chip, const std::vector<uint32_t>& segmentLens, bool shaAppended) {
  std::vector<uint8_t> image(24, 0);
  image[0] = 0xE9;
  image[1] = static_cast<uint8_t>(segmentLens.size());
  std::memcpy(&image[12], &chip, sizeof(chip));
  image[23] = shaAppended ? 1 : 0;
  for (const uint32_t len : segmentLens) {
    uint8_t seg[8] = {};
    std::memcpy(seg + 4, &len, sizeof(len));
    image.insert(image.end(), seg, seg + 8);
    image.insert(image.end(), len, 0x5A);
  }
  image.resize((image.size() + 16) & ~static_cast<size_t>(15), 0);
  if (shaAppended) image.resize(image.size() + 32, 0);
  return image;
}

bool readVector(uint32_t offset, void* dst, uint32_t len, void* ctx) {
  const auto* image = static_cast<const std::vector<uint8_t>*>(ctx);
  if (offset > image->size() || len > image->size() - offset) return false;
  std::memcpy(dst, image->data() + offset, len);
  return true;
}

bool holds(std::vector<uint8_t> slot, uint32_t slotSize, uint16_t chip = kChipS3) {
  return slotHoldsAppImage(readVector, &slot, slotSize, chip);
}

}  // namespace

TEST(OtaSlotModel, SlotImageCheckAcceptsAWellFormedImage) {
  const auto image = appImage(kChipS3, {100, 4096, 7}, true);
  EXPECT_TRUE(holds(image, static_cast<uint32_t>(image.size())));
  EXPECT_TRUE(holds(appImage(kChipS3, {64}, false), 0x1000));
}

TEST(OtaSlotModel, SlotImageCheckRejectsErasedWrongChipAndTruncatedSlots) {
  EXPECT_FALSE(holds(std::vector<uint8_t>(4096, 0xFF), 4096));
  EXPECT_FALSE(holds(appImage(5, {100}, true), 4096));

  auto noSegments = appImage(kChipS3, {100}, true);
  noSegments[1] = 0;
  EXPECT_FALSE(holds(noSegments, 4096));

  auto tooManySegments = appImage(kChipS3, {100}, true);
  tooManySegments[1] = 17;
  EXPECT_FALSE(holds(tooManySegments, 4096));

  const auto image = appImage(kChipS3, {100, 4096}, true);
  EXPECT_FALSE(holds(image, static_cast<uint32_t>(image.size() - 1)));

  // A segment length read from erased flash runs past the slot.
  auto erasedTail = appImage(kChipS3, {100, 4096}, true);
  std::memset(&erasedTail[24 + 8 + 100], 0xFF, erasedTail.size() - (24 + 8 + 100));
  EXPECT_FALSE(holds(erasedTail, static_cast<uint32_t>(erasedTail.size())));
}
