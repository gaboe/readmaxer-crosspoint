#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "GuestSlot.h"
#include "Runtime.h"
#include "SdGuestUpdate.h"
#include "SdGuestUpdateModel.h"

namespace {
sd_guest_update::Manifest verifiedManifest{};
}

namespace sd_guest_update {
Result parseAndVerify(const uint8_t*, size_t, const char*, uint32_t, Manifest& manifest) {
  manifest = verifiedManifest;
  return Result::Ok;
}
const char* resultName(Result) { return "TEST"; }
}  // namespace sd_guest_update

namespace guest_slot {
Inspection inspectFresh() {
  Inspection result;
  result.image.installed = true;
  result.partition = &fake::destination;
  return result;
}
}  // namespace guest_slot

TEST(SdGuestUpdateRuntime, SameShaSkipsFlashWriteAndClearsFiles) {
  fake::files.clear();
  fake::flash.assign(fake::destination.size, 0xa5);
  fake::pending = fake::samePartition = fake::failRead = false;
  fake::expectGuardedRead = false;
  fake::erases = fake::writes = fake::selections = fake::restarts = 0;
  const std::vector<uint8_t> image = {'s', 'a', 'm', 'e', '-', 'i', 'm', 'a', 'g', 'e'};
  fake::files["/readmaxer/update.json"] = {'{', '}'};
  fake::files["/readmaxer/update.bin"] = image;
  std::memcpy(fake::flash.data(), image.data(), image.size());
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  ASSERT_NE(ctx, nullptr);
  ASSERT_EQ(EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr), 1);
  ASSERT_EQ(EVP_DigestUpdate(ctx, image.data(), image.size()), 1);
  ASSERT_EQ(EVP_DigestFinal_ex(ctx, verifiedManifest.sha, nullptr), 1);
  EVP_MD_CTX_free(ctx);
  verifiedManifest.size = image.size();
  for (unsigned i = 0; i < 32; ++i) std::snprintf(verifiedManifest.shaHex + 2 * i, 3, "%02x", verifiedManifest.sha[i]);

  sd_guest_update::installPending(nullptr);

  EXPECT_EQ(fake::erases, 0);
  EXPECT_EQ(fake::writes, 0);
  EXPECT_EQ(fake::selections, 1);
  EXPECT_EQ(fake::restarts, 1);
  EXPECT_FALSE(Storage.exists("/readmaxer/update.json"));
  EXPECT_FALSE(Storage.exists("/readmaxer/update.bin"));
}
