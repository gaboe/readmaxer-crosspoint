#include <gtest/gtest.h>
#include <monocypher-ed25519.h>
#include <openssl/evp.h>

#include <array>
#include <cstring>
#include <string>

#include "SdGuestUpdateModel.h"

namespace {
std::array<uint8_t, 32> testSeed() {
  std::array<uint8_t, 32> seed{};
  for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(i);
  return seed;
}

std::string signedManifest(uint8_t (&publicKey)[32]) {
  auto seed = testSeed();
  uint8_t secretKey[64];
  crypto_ed25519_key_pair(secretKey, publicKey, seed.data());
  constexpr char message[] =
      "readmaxer-guest-update-v1\nsr-001122334455\n"
      "0000000000000000000000000000000000000000000000000000000000000000\n4096\n"
      "0123456789abcdef0123456789abcdef01234567\n";
  uint8_t signature[64];
  crypto_ed25519_sign(signature, secretKey, reinterpret_cast<const uint8_t*>(message), strlen(message));
  char encoded[89]{};
  EXPECT_EQ(EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded), signature, sizeof(signature)), 88);
  return std::string("{\"v\":1,\"device_id\":\"sr-001122334455\",\"sha256\":\"") +
         "0000000000000000000000000000000000000000000000000000000000000000\",\"size\":4096,\"version\":\"" +
         "0123456789abcdef0123456789abcdef01234567\",\"sig\":\"" + encoded + "\"}";
}

sd_guest_update::Result parse(const std::string& text, const uint8_t (&key)[32], const char* device = "sr-001122334455",
                              uint32_t limit = 8192) {
  sd_guest_update::Manifest manifest{};
  return sd_guest_update::parseAndVerifyWithKey(reinterpret_cast<const uint8_t*>(text.data()), text.size(), device,
                                                limit, key, manifest);
}
}  // namespace

TEST(SdGuestUpdate, SignedManifestAndRefusals) {
  uint8_t publicKey[32];
  const auto json = signedManifest(publicKey);
  EXPECT_EQ(parse(json, publicKey), sd_guest_update::Result::Ok);
  EXPECT_EQ(parse(json, publicKey, "sr-001122334456"), sd_guest_update::Result::WrongDevice);
  EXPECT_EQ(parse(json, publicKey, "sr-001122334455", 4095), sd_guest_update::Result::TooLarge);
  auto damaged = json;
  damaged.replace(damaged.find("4096"), 4, "4097");
  EXPECT_EQ(parse(damaged, publicKey), sd_guest_update::Result::BadSignature);
  damaged = json;
  damaged.replace(damaged.find("\"v\":1"), 5, "\"v\":2");
  EXPECT_EQ(parse(damaged, publicKey), sd_guest_update::Result::BadManifest);
}

TEST(SdGuestUpdate, Rfc8032EmptyMessage) {
  const uint8_t publicKey[32] = {0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe,
                                 0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6,
                                 0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
  const uint8_t signature[64] = {0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72, 0x90, 0x86, 0xe2, 0xcc, 0x80,
                                 0x6e, 0x82, 0x8a, 0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74, 0xd8, 0x73,
                                 0xe0, 0x65, 0x22, 0x49, 0x01, 0x55, 0x5f, 0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b,
                                 0xac, 0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b, 0xd2, 0x5b, 0xf5, 0xf0,
                                 0x59, 0x5b, 0xbe, 0x24, 0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10, 0x0b};
  EXPECT_EQ(crypto_ed25519_check(signature, publicKey, nullptr, 0), 0);
  const uint8_t wrong[] = {'x'};
  EXPECT_NE(crypto_ed25519_check(signature, publicKey, wrong, sizeof(wrong)), 0);
}

TEST(SdGuestUpdate, ServerGuestSharedVector) {
  uint8_t seed[32];
  std::memset(seed, 7, sizeof(seed));
  uint8_t secretKey[64];
  uint8_t publicKey[32];
  crypto_ed25519_key_pair(secretKey, publicKey, seed);
  const std::string json =
      "{\"v\":1,\"device_id\":\"sr-98c377bea840\",\"sha256\":\""
      "9febe01bd41bfb69683e29d711d8adffc9ae38de17a6873464b416f3b67398b6\",\"size\":10,"
      "\"version\":\"0123456789abcdef0123456789abcdef01234567\",\"sig\":\""
      "HoQAZ4HbIwqPnsVfLsQwmrklSg/HhB5C8nRRdD7xWYnJMlGymJM1LkODxjQbPeK1aUFeZwI1z9Xf+Gt50zebDQ==\"}";
  EXPECT_EQ(parse(json, publicKey, "sr-98c377bea840"), sd_guest_update::Result::Ok);
}
