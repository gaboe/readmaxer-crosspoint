#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "GuestInstall.h"
#include "GuestInstallModel.h"
#include "OtaBootSwitch.h"
#include "Runtime.h"

namespace fake {
inline ota_boot::SelectionResult selection = ota_boot::SelectionResult::Selected;
}

namespace ota_boot {
AppWriteGuard::AppWriteGuard(const esp_partition_t*) {
  acquired = fake::guardAllowed;
  fake::guardHeld = acquired;
  if (fake::changeSource) fake::files.at("/readmaxer/install.bin")[512] ^= 1;
}
AppWriteGuard::~AppWriteGuard() { fake::guardHeld = false; }
bool confirmationPending() { return fake::pending; }
SlotCheck runningSlotCheck() { return SlotCheck::Confirmed; }
SelectionResult switchTo(const esp_partition_t* dest, uint32_t state) {
  EXPECT_EQ(dest, &fake::destination);
  EXPECT_FALSE(fake::guardHeld);
  ++fake::selections;
  fake::selectedState = state;
  return fake::selection;
}
}  // namespace ota_boot

namespace guest_slot {
void invalidateCache() {}
}  // namespace guest_slot

namespace {
using guest_install::Result;
using Bytes = std::vector<uint8_t>;
constexpr char MAC[] = "98c377bea840";

struct Hash {
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  Hash() { EXPECT_EQ(EVP_DigestInit_ex(context, EVP_sha256(), nullptr), 1); }
  ~Hash() { EVP_MD_CTX_free(context); }
  ota_boot::ImageHasher get() {
    return {context,
            [](void* ctx, const uint8_t* data, uint32_t len) {
              return EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(ctx), data, len) == 1;
            },
            [](void* ctx, uint8_t (&out)[32]) {
              return EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(ctx), out, nullptr) == 1;
            }};
  }
};

void digest(const Bytes& bytes, uint8_t (&out)[32]) {
  Hash hash;
  ASSERT_TRUE(hash.get().update(hash.context, bytes.data(), bytes.size()));
  ASSERT_TRUE(hash.get().finish(hash.context, out));
}

void put32(Bytes& image, size_t pos, uint32_t value) { std::memcpy(image.data() + pos, &value, 4); }

void seal(Bytes& image) {
  size_t pos = 24;
  uint8_t checksum = 0xef;
  for (unsigned i = 0; i < image[1]; ++i) {
    uint32_t len;
    std::memcpy(&len, image.data() + pos + 4, 4);
    pos += 8;
    for (size_t j = 0; j < len; ++j) checksum ^= image[pos++];
  }
  image.resize((pos + 16) & ~size_t{15}, 0);
  image.back() = checksum;
  uint8_t sha[32];
  digest(image, sha);
  image.insert(image.end(), sha, sha + 32);
}

Bytes image(const char* markers = "READMAXER-GUEST-V1; SR_98c377bea840", uint32_t segmentBytes = 8192) {
  Bytes bytes(32 + segmentBytes + 12, 0);
  bytes[0] = 0xe9;
  bytes[1] = 2;
  bytes[12] = 9;
  bytes[23] = 1;
  put32(bytes, 24, 0x3c000020);
  put32(bytes, 28, segmentBytes);
  put32(bytes, 32, 0xabcd5432);
  std::memcpy(bytes.data() + 48, "test", 5);
  constexpr char project[] = "readmaxer-xteink-x4pro";
  std::memcpy(bytes.data() + 80, project, sizeof(project));
  std::memcpy(bytes.data() + 509, markers, std::strlen(markers));
  put32(bytes, 32 + segmentBytes, 0x42002028);
  put32(bytes, 36 + segmentBytes, 4);
  seal(bytes);
  return bytes;
}

bool read(uint32_t pos, void* out, uint32_t len, void* ctx) {
  const auto& bytes = *static_cast<Bytes*>(ctx);
  if (pos > bytes.size() || len > bytes.size() - pos) return false;
  std::memcpy(out, bytes.data() + pos, len);
  return true;
}

Result validate(Bytes& bytes, bool wrongSha = false, const char* mac = MAC, uint32_t capacity = 0x5c0000,
                uint32_t chunk = 512) {
  uint8_t expected[32];
  digest(bytes, expected);
  if (wrongSha) expected[0] ^= 1;
  Hash imageHash, fileHash;
  std::vector<uint8_t> buffer(chunk);
  return guest_install::validate(read, &bytes, bytes.size(), capacity, mac, expected, imageHash.get(), fileHash.get(),
                                 buffer.data(), buffer.size());
}
}  // namespace

TEST(GuestInstall, GoodSyntheticImage) {
  auto bytes = image();
  for (uint32_t chunk : {32, 64, 512, 4096}) EXPECT_EQ(validate(bytes, false, MAC, 0x5c0000, chunk), Result::Ok);
}

TEST(GuestInstall, WrongSha) {
  auto bytes = image();
  EXPECT_EQ(validate(bytes, true), Result::WrongSha);
}

TEST(GuestInstall, WrongMac) {
  auto bytes = image();
  EXPECT_EQ(validate(bytes, false, "98c377bea841"), Result::BadIdentity);
}

TEST(GuestInstall, IdentityRefusals) {
  for (const char* markers :
       {"READMAXER-GUEST-V1;READMAXER-GUEST-V1;SR_98c377bea840", "SR_98c377bea840",
        "READMAXER-GUEST-V1;READMAXER-HOST-V1;SR_98c377bea840", "READMAXER-GUEST-V1;SR_98c377bea840 SR_98c377bea841",
        "READMAXER-GUEST-V1;SR_98c377bea840 SR_98c377bea840", "READMAXER-GUEST-V1;SR_98C377BEA840",
        "READMAXER-GUEST-V1;", "READMAXER-GUEST-V1;SR_98c3"}) {
    auto bytes = image(markers);
    EXPECT_EQ(validate(bytes), Result::BadIdentity) << markers;
  }
}

TEST(GuestInstall, WrongProject) {
  auto bytes = image();
  bytes[80] = 'X';
  seal(bytes);
  EXPECT_EQ(validate(bytes), Result::WrongProject);
}

TEST(GuestInstall, Oversized) {
  auto bytes = image();
  EXPECT_EQ(validate(bytes, false, MAC, bytes.size() - 1), Result::TooLarge);
}

TEST(GuestInstall, SharedImageRules) {
  auto bytes = image();
  bytes[24] ^= 4;
  seal(bytes);
  EXPECT_EQ(validate(bytes), Result::BadImage);
  bytes = image();
  bytes.back() ^= 1;
  EXPECT_EQ(validate(bytes), Result::BadImage);
  bytes = image();
  bytes.push_back(0);
  EXPECT_EQ(validate(bytes), Result::BadImage);
}

TEST(GuestInstall, ManifestShaParsing) {
  uint8_t sha[32];
  EXPECT_TRUE(guest_install::parseSha("d96f38595344288c00d016030289e48329ab6e14ea4b6feab175f69cc301758b", sha));
  EXPECT_EQ(sha[0], 0xd9);
  EXPECT_EQ(sha[31], 0x8b);
  EXPECT_FALSE(guest_install::parseSha(nullptr, sha));
  EXPECT_FALSE(guest_install::parseSha("d96f", sha));
  EXPECT_FALSE(guest_install::parseSha("x96f38595344288c00d016030289e48329ab6e14ea4b6feab175f69cc301758b", sha));
}

TEST(GuestInstall, GuardRefusesWithoutValidNonGuestOutsideDestination) {
  using namespace ota_boot;
  SelectEntry entries[2]{};
  for (auto& e : entries) std::memset(&e, 0xff, sizeof(e));
  BootableSlots images{{true, false}};
  EXPECT_FALSE(appEraseIsSafe(entries, images, 1));
  entries[0].ota_seq = 1;
  entries[0].ota_state = kOtaImgValid;
  entries[0].crc = computeSeqCrc(1);
  EXPECT_TRUE(appEraseIsSafe(entries, images, 1));
  EXPECT_FALSE(appEraseIsSafe(entries, images, 0));
  images.slots[0] = false;
  EXPECT_FALSE(appEraseIsSafe(entries, images, 1));
}

TEST(GuestInstall, RealRm3Image) {
  const char* path = std::getenv("CP3_GUEST_IMAGE");
  if (!path) GTEST_SKIP() << "Set CP3_GUEST_IMAGE to the saved RM3 x4pro-guest.bin";
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file.good());
  Bytes bytes(std::istreambuf_iterator<char>(file), {});
  ASSERT_FALSE(bytes.empty());
  uint8_t expected[32], actual[32];
  ASSERT_TRUE(guest_install::parseSha("d96f38595344288c00d016030289e48329ab6e14ea4b6feab175f69cc301758b", expected));
  digest(bytes, actual);
  ASSERT_EQ(std::memcmp(expected, actual, 32), 0);
  EXPECT_EQ(validate(bytes), Result::Ok);
  EXPECT_EQ(validate(bytes, false, "98c377bea841"), Result::BadIdentity);
}

class GuestInstallProduction : public testing::Test {
 protected:
  char sha[65]{};
  guest_install::Timing timing;
  void SetUp() override {
    fake::files.clear();
    fake::flash.assign(fake::destination.size, 0xa5);
    fake::guardAllowed = true;
    fake::guardHeld = fake::pending = fake::samePartition = fake::corruptReadback = false;
    fake::failWrite = fake::failRead = fake::changeSource = fake::failReceiptClose = false;
    fake::failReceiptRename = false;
    fake::erases = fake::writes = fake::selections = 0;
    fake::erasedBytes = 0;
    fake::selection = ota_boot::SelectionResult::Selected;
    prepare(image());
  }
  void prepare(Bytes bytes) {
    uint8_t hash[32];
    digest(bytes, hash);
    for (unsigned i = 0; i < 32; ++i) snprintf(sha + 2 * i, 3, "%02x", hash[i]);
    const auto manifest = std::string("{\"sha256\":\"") + sha + "\"}";
    fake::files["/readmaxer/install.json"] = Bytes(manifest.begin(), manifest.end());
    fake::files["/readmaxer/install.bin"] = std::move(bytes);
  }
  void expectUntouched() {
    EXPECT_EQ(fake::erases, 0);
    EXPECT_EQ(fake::writes, 0);
    EXPECT_EQ(fake::selections, 0);
    EXPECT_TRUE(std::all_of(fake::flash.begin(), fake::flash.end(), [](uint8_t b) { return b == 0xa5; }));
  }
};

TEST_F(GuestInstallProduction, GuardRefusalLeavesFlashAndSelectionUntouched) {
  fake::guardAllowed = false;
  EXPECT_EQ(guest_install::install(sha, timing), Result::GuardRefused);
  expectUntouched();
}

TEST_F(GuestInstallProduction, PendingConfirmationLeavesFlashAndSelectionUntouched) {
  fake::pending = true;
  EXPECT_EQ(guest_install::install(sha, timing), Result::GuardRefused);
  expectUntouched();
}

TEST_F(GuestInstallProduction, RunningDestinationLeavesFlashAndSelectionUntouched) {
  fake::samePartition = true;
  EXPECT_EQ(guest_install::install(sha, timing), Result::NoPartition);
  expectUntouched();
}

TEST_F(GuestInstallProduction, InvalidImagesNeverReachErase) {
  for (const char* markers :
       {"", "READMAXER-GUEST-V1;SR_98c377bea841", "READMAXER-GUEST-V1;READMAXER-GUEST-V1;SR_98c377bea840",
        "READMAXER-GUEST-V1;READMAXER-HOST-V1;SR_98c377bea840"}) {
    prepare(image(markers));
    EXPECT_EQ(guest_install::install(sha, timing), Result::BadIdentity);
    expectUntouched();
  }
  auto bytes = image();
  bytes[80] = 'X';
  seal(bytes);
  prepare(bytes);
  EXPECT_EQ(guest_install::install(sha, timing), Result::WrongProject);
  expectUntouched();
  bytes.resize(fake::destination.size + 1);
  prepare(bytes);
  EXPECT_EQ(guest_install::install(sha, timing), Result::TooLarge);
  expectUntouched();
}

TEST_F(GuestInstallProduction, WrongManifestNeverReachesErase) {
  auto& json = fake::files.at("/readmaxer/install.json");
  const std::string wrong = "{\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}";
  json.assign(wrong.begin(), wrong.end());
  EXPECT_EQ(guest_install::install(sha, timing), Result::WrongSha);
  expectUntouched();
  json.assign(513, ' ');
  EXPECT_EQ(guest_install::install(sha, timing), Result::BadManifest);
  expectUntouched();
  json = {'{', '}'};
  EXPECT_EQ(guest_install::install(sha, timing), Result::BadManifest);
  expectUntouched();
}

TEST_F(GuestInstallProduction, GoodImageWritesAndSelectsNewGuest) {
  const auto expected = fake::files.at("/readmaxer/install.bin");
  EXPECT_EQ(guest_install::install(sha, timing), Result::Ok);
  EXPECT_EQ(fake::selections, 1);
  EXPECT_EQ(fake::selectedState, ota_boot::kOtaImgNew);
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), fake::flash.begin()));
  EXPECT_EQ(fake::erases, (expected.size() + 65535) / 65536);
  EXPECT_EQ(fake::erasedBytes, ((expected.size() + 65535) / 65536) * 65536);
  EXPECT_TRUE(guest_install::writeReceipt(sha, Result::Ok, timing));
  const auto& receipt = fake::files.at("/readmaxer/last-install.json");
  const std::string text(receipt.begin(), receipt.end());
  EXPECT_NE(text.find(sha), std::string::npos);
  EXPECT_NE(text.find("\"result\":\"OK\""), std::string::npos);
  EXPECT_NE(text.find("\"time\":"), std::string::npos);
}

TEST_F(GuestInstallProduction, LargeImageErasesEach64KiBBlock) {
  const auto expected = image("READMAXER-GUEST-V1; SR_98c377bea840", 73728);
  prepare(expected);
  EXPECT_EQ(guest_install::install(sha, timing), Result::Ok);
  EXPECT_EQ(fake::erases, 2);
  EXPECT_EQ(fake::erasedBytes, 131072u);
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), fake::flash.begin()));
  EXPECT_EQ(fake::selections, 1);
}

TEST_F(GuestInstallProduction, ReadbackMismatchNeverSelects) {
  fake::corruptReadback = true;
  EXPECT_EQ(guest_install::install(sha, timing), Result::BadReadback);
  EXPECT_GT(fake::erases, 0);
  EXPECT_EQ(fake::selections, 0);
}

TEST_F(GuestInstallProduction, SourceChangedAfterValidationNeverSelects) {
  fake::changeSource = true;
  EXPECT_EQ(guest_install::install(sha, timing), Result::BadReadback);
  EXPECT_EQ(fake::selections, 0);
}

TEST_F(GuestInstallProduction, WriteFailureNeverSelects) {
  fake::failWrite = true;
  EXPECT_EQ(guest_install::install(sha, timing), Result::WriteFailed);
  EXPECT_EQ(fake::selections, 0);
  EXPECT_FALSE(fake::guardHeld);
}

TEST_F(GuestInstallProduction, SelectionUnknownIsNotSuccess) {
  fake::selection = ota_boot::SelectionResult::Unknown;
  EXPECT_EQ(guest_install::install(sha, timing), Result::SelectionUnknown);
}

TEST_F(GuestInstallProduction, ReceiptCloseFailureDoesNotReplaceOldReceipt) {
  fake::files["/readmaxer/last-install.json"] = {'o', 'l', 'd'};
  fake::failReceiptClose = true;
  EXPECT_FALSE(guest_install::writeReceipt(sha, Result::Ok, timing));
  EXPECT_EQ(fake::files.at("/readmaxer/last-install.json"), (Bytes{'o', 'l', 'd'}));
}

TEST_F(GuestInstallProduction, ReceiptRenameFailureRestoresOldReceipt) {
  fake::files["/readmaxer/last-install.json"] = {'o', 'l', 'd'};
  fake::failReceiptRename = true;
  EXPECT_FALSE(guest_install::writeReceipt(sha, Result::Ok, timing));
  EXPECT_EQ(fake::files.at("/readmaxer/last-install.json"), (Bytes{'o', 'l', 'd'}));
  EXPECT_FALSE(fake::files.contains("/readmaxer/last-install.bak"));
  fake::failReceiptRename = false;
  EXPECT_TRUE(guest_install::writeReceipt(sha, Result::Ok, timing));
  EXPECT_NE(fake::files.at("/readmaxer/last-install.json"), (Bytes{'o', 'l', 'd'}));
  EXPECT_FALSE(fake::files.contains("/readmaxer/last-install.bak"));
}

TEST_F(GuestInstallProduction, RealRm3Install) {
  const char* path = std::getenv("CP3_GUEST_IMAGE");
  if (!path) GTEST_SKIP() << "Set CP3_GUEST_IMAGE to the saved RM3 x4pro-guest.bin";
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file.good());
  Bytes bytes(std::istreambuf_iterator<char>(file), {});
  prepare(bytes);
  EXPECT_EQ(guest_install::install(sha, timing), Result::Ok);
  EXPECT_EQ(fake::selections, 1);
  EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), fake::flash.begin()));
}
