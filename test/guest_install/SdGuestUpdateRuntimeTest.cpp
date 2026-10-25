#include <gtest/gtest.h>
#include <monocypher-ed25519.h>
#include <openssl/evp.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "GuestSlot.h"
#include "Runtime.h"
#include "SdGuestUpdate.h"
#include "SdGuestUpdateModel.h"

namespace {
using Bytes = std::vector<uint8_t>;
constexpr char deviceId[] = "sr-98c377bea840";
constexpr char version[] = "0123456789abcdef0123456789abcdef01234567";

std::array<uint8_t, 32> testSeed() {
  std::array<uint8_t, 32> seed{};
  for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(i + 1);
  return seed;
}

void testKeyPair(uint8_t (&secret)[64], uint8_t (&publicKey)[32]) {
  auto seed = testSeed();
  crypto_ed25519_key_pair(secret, publicKey, seed.data());
}

Bytes guestImage(uint8_t marker, size_t segmentSize = 8192) {
  Bytes image(32 + segmentSize + 12, 0);
  image[0] = 0xe9;
  image[1] = 2;
  image[12] = 9;
  image[23] = 1;
  const uint32_t address = 0x3c000020;
  const uint32_t length = static_cast<uint32_t>(segmentSize);
  const uint32_t descriptorMagic = 0xabcd5432;
  std::memcpy(image.data() + 24, &address, sizeof(address));
  std::memcpy(image.data() + 28, &length, sizeof(length));
  std::memcpy(image.data() + 32, &descriptorMagic, sizeof(descriptorMagic));
  std::memcpy(image.data() + 48, "test", 5);
  constexpr char project[] = "readmaxer-xteink-x4pro";
  std::memcpy(image.data() + 80, project, sizeof(project));
  image[200] = marker;
  constexpr char guestMarker[] = "READMAXER-GUEST-V1; SR_98c377bea840";
  std::memcpy(image.data() + 509, guestMarker, sizeof(guestMarker) - 1);
  const uint32_t secondAddress = 0x42002028;
  const uint32_t secondLength = 4;
  std::memcpy(image.data() + 32 + segmentSize, &secondAddress, sizeof(secondAddress));
  std::memcpy(image.data() + 36 + segmentSize, &secondLength, sizeof(secondLength));
  size_t pos = 24;
  uint8_t checksum = 0xef;
  for (unsigned segment = 0; segment < image[1]; ++segment) {
    uint32_t segmentLength;
    std::memcpy(&segmentLength, image.data() + pos + 4, sizeof(segmentLength));
    pos += 8;
    for (uint32_t i = 0; i < segmentLength; ++i) checksum ^= image[pos++];
  }
  image.resize((pos + 16) & ~size_t{15}, 0);
  image.back() = checksum;
  uint8_t digest[32];
  unsigned digestSize = sizeof(digest);
  EXPECT_EQ(EVP_Digest(image.data(), image.size(), digest, &digestSize, EVP_sha256(), nullptr), 1);
  EXPECT_EQ(digestSize, sizeof(digest));
  image.insert(image.end(), digest, digest + sizeof(digest));
  return image;
}

std::string shaHex(const Bytes& image) {
  uint8_t digest[32];
  unsigned digestSize = sizeof(digest);
  EXPECT_EQ(EVP_Digest(image.data(), image.size(), digest, &digestSize, EVP_sha256(), nullptr), 1);
  EXPECT_EQ(digestSize, sizeof(digest));
  char text[65];
  for (size_t i = 0; i < sizeof(digest); ++i) std::snprintf(text + 2 * i, 3, "%02x", digest[i]);
  return text;
}

std::string signedManifest(const Bytes& image) {
  uint8_t secretKey[64];
  uint8_t publicKey[32];
  testKeyPair(secretKey, publicKey);
  const auto sha = shaHex(image);
  char message[192];
  const int messageLength = std::snprintf(message, sizeof(message), "readmaxer-guest-update-v1\n%s\n%s\n%lu\n%s\n",
                                          deviceId, sha.c_str(), static_cast<unsigned long>(image.size()), version);
  EXPECT_GT(messageLength, 0);
  EXPECT_LT(static_cast<size_t>(messageLength), sizeof(message));
  uint8_t signature[64];
  crypto_ed25519_sign(signature, secretKey, reinterpret_cast<const uint8_t*>(message), messageLength);
  char encoded[89]{};
  EXPECT_EQ(EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded), signature, sizeof(signature)), 88);
  char json[384];
  const int jsonLength =
      std::snprintf(json, sizeof(json),
                    "{\"v\":1,\"device_id\":\"%s\",\"sha256\":\"%s\",\"size\":%lu,"
                    "\"version\":\"%s\",\"sig\":\"%s\"}",
                    deviceId, sha.c_str(), static_cast<unsigned long>(image.size()), version, encoded);
  EXPECT_GT(jsonLength, 0);
  EXPECT_LT(static_cast<size_t>(jsonLength), sizeof(json));
  return std::string(json, jsonLength);
}

Bytes bytes(const std::string& text) { return Bytes(text.begin(), text.end()); }

void installFiles(const Bytes& image, const std::string& manifest) {
  fake::files["/readmaxer/update.json"] = bytes(manifest);
  fake::files["/readmaxer/update.bin"] = image;
}

void resetRuntime() {
  fake::files.clear();
  fake::flash.assign(fake::destination.size, 0xff);
  fake::guardAllowed = true;
  fake::guardHeld = false;
  fake::pending = fake::samePartition = fake::failRead = fake::failRemove = false;
  fake::replaceSdFilesAfterPartitionRead = false;
  fake::replacementManifest.clear();
  fake::replacementImage.clear();
  fake::slotCheck = ota_boot::SlotCheck::Confirmed;
  fake::selectedPartition = nullptr;
  fake::destination.subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
  fake::expectGuardedRead = false;
  fake::erases = fake::writes = fake::selections = fake::restarts = 0;
}
}  // namespace

namespace sd_guest_update {
Result parseAndVerify(const uint8_t* json, size_t length, const char* device, uint32_t maximumSize,
                      Manifest& manifest) {
  uint8_t secretKey[64];
  uint8_t publicKey[32];
  testKeyPair(secretKey, publicKey);
  return parseAndVerifyWithKey(json, length, device, maximumSize, publicKey, manifest);
}
}  // namespace sd_guest_update

namespace guest_slot {
Inspection inspectFresh() {
  Inspection result;
  result.image.installed = true;
  result.partition = &fake::destination;
  return result;
}
}  // namespace guest_slot

TEST(SdGuestUpdateRuntime, SignedSameShaSkipsFlashWriteAndClearsFiles) {
  resetRuntime();
  const auto image = guestImage(0x31);
  installFiles(image, signedManifest(image));
  std::memcpy(fake::flash.data(), image.data(), image.size());

  sd_guest_update::installPending(nullptr);

  EXPECT_EQ(fake::erases, 0);
  EXPECT_EQ(fake::writes, 0);
  EXPECT_EQ(fake::selections, 1);
  EXPECT_EQ(fake::restarts, 1);
  EXPECT_FALSE(Storage.exists("/readmaxer/update.json"));
  EXPECT_FALSE(Storage.exists("/readmaxer/update.bin"));
}

TEST(SdGuestUpdateRuntime, SdReplacementAfterVerificationCannotChangeInstalledImage) {
  resetRuntime();
  const auto verifiedImage = guestImage(0x31);
  const auto replacementImage = guestImage(0x72, 12288);
  installFiles(verifiedImage, signedManifest(verifiedImage));
  fake::replacementManifest = bytes(signedManifest(replacementImage));
  fake::replacementImage = replacementImage;
  fake::replaceSdFilesAfterPartitionRead = true;

  sd_guest_update::installPending(nullptr);

  EXPECT_EQ(fake::erases, 0);
  EXPECT_EQ(fake::writes, 0);
  EXPECT_EQ(fake::selections, 0);
  EXPECT_EQ(fake::restarts, 0);
  EXPECT_FALSE(Storage.exists("/readmaxer/update.json"));
}

TEST(SdGuestUpdateRuntime, UnsignedSameSizeReplacementCannotChangeInstalledImage) {
  resetRuntime();
  const auto verifiedImage = guestImage(0x31);
  const auto replacementImage = guestImage(0x72);
  installFiles(verifiedImage, signedManifest(verifiedImage));
  auto replacementManifest = signedManifest(replacementImage);
  const size_t signature = replacementManifest.find("\"sig\":\"") + 7;
  replacementManifest[signature] = replacementManifest[signature] == 'A' ? 'B' : 'A';
  fake::replacementManifest = bytes(replacementManifest);
  fake::replacementImage = replacementImage;
  fake::replaceSdFilesAfterPartitionRead = true;

  sd_guest_update::installPending(nullptr);

  EXPECT_EQ(fake::erases, 0);
  EXPECT_EQ(fake::writes, 0);
  EXPECT_EQ(fake::selections, 0);
  EXPECT_EQ(fake::restarts, 0);
  EXPECT_FALSE(Storage.exists("/readmaxer/update.json"));
}

TEST(SdGuestUpdateRuntime, RefusesOta0BeforeSameShaNoOp) {
  resetRuntime();
  const auto image = guestImage(0x31);
  installFiles(image, signedManifest(image));
  std::memcpy(fake::flash.data(), image.data(), image.size());
  fake::destination.subtype = 0x10;

  sd_guest_update::installPending(nullptr);

  EXPECT_EQ(fake::erases, 0);
  EXPECT_EQ(fake::writes, 0);
  EXPECT_EQ(fake::selections, 0);
  EXPECT_EQ(fake::restarts, 0);
  EXPECT_FALSE(Storage.exists("/readmaxer/update.json"));
}

TEST(SdGuestUpdateRuntime, FailedManifestDeleteDoesNotRestartGuest) {
  resetRuntime();
  const auto image = guestImage(0x31);
  installFiles(image, signedManifest(image));
  std::memcpy(fake::flash.data(), image.data(), image.size());
  fake::failRemove = true;

  sd_guest_update::installPending(nullptr);

  EXPECT_EQ(fake::selections, 0);
  EXPECT_EQ(fake::selectedPartition, nullptr);
  EXPECT_EQ(fake::restarts, 0);
  EXPECT_TRUE(Storage.exists("/readmaxer/update.json"));
}

TEST(SdGuestUpdateRuntime, FailedManifestDeleteKeepsCrossPointSelectedAfterInstall) {
  resetRuntime();
  const auto image = guestImage(0x31);
  installFiles(image, signedManifest(image));
  fake::failRemove = true;

  sd_guest_update::installPending(nullptr);

  EXPECT_GT(fake::erases, 0);
  EXPECT_GT(fake::writes, 0);
  EXPECT_EQ(fake::selections, 0);
  EXPECT_EQ(fake::selectedPartition, nullptr);
  EXPECT_EQ(fake::slotCheck, ota_boot::SlotCheck::Confirmed);
  EXPECT_EQ(fake::restarts, 0);
  EXPECT_TRUE(Storage.exists("/readmaxer/update.json"));
}

TEST(SdGuestUpdateRuntime, SuccessfulInstallClearsFilesAndSelectsGuest) {
  resetRuntime();
  const auto image = guestImage(0x31);
  installFiles(image, signedManifest(image));

  sd_guest_update::installPending(nullptr);

  EXPECT_GT(fake::erases, 0);
  EXPECT_GT(fake::writes, 0);
  EXPECT_EQ(fake::selections, 1);
  EXPECT_EQ(fake::selectedPartition, &fake::destination);
  EXPECT_EQ(fake::restarts, 1);
  EXPECT_FALSE(Storage.exists("/readmaxer/update.json"));
  EXPECT_FALSE(Storage.exists("/readmaxer/update.bin"));
}

TEST(SdGuestUpdateRuntime, FellBackSlotRefusesAndClearsManifest) {
  resetRuntime();
  const auto image = guestImage(0x31);
  installFiles(image, signedManifest(image));
  fake::slotCheck = ota_boot::SlotCheck::FellBack;

  sd_guest_update::installPending(nullptr);

  EXPECT_EQ(fake::erases, 0);
  EXPECT_EQ(fake::writes, 0);
  EXPECT_EQ(fake::selections, 0);
  EXPECT_EQ(fake::restarts, 0);
  EXPECT_FALSE(Storage.exists("/readmaxer/update.json"));
}
