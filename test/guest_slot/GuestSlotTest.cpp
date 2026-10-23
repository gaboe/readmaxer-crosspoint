#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "FirmwareHostTag.h"
#include "GuestDevLoop.h"
#include "GuestSlotModel.h"

namespace {

bool readBytes(uint32_t offset, void* dst, uint32_t len, void* ctx) {
  const auto& bytes = *static_cast<const std::vector<uint8_t>*>(ctx);
  if (offset > bytes.size() || len > bytes.size() - offset) return false;
  std::memcpy(dst, bytes.data() + offset, len);
  return true;
}

guest_slot::GuestImage inspect(std::vector<uint8_t>& bytes) {
  return guest_slot::inspectHeader(readBytes, &bytes, bytes.size());
}

std::vector<uint8_t> image(const char* project) {
  std::vector<uint8_t> bytes(32 + 256, 0);
  bytes[0] = 0xE9;
  bytes[1] = 1;
  const uint32_t length = 256;
  std::memcpy(bytes.data() + 28, &length, 4);
  const uint32_t magic = 0xABCD5432;
  std::memcpy(bytes.data() + 32, &magic, 4);
  std::memcpy(bytes.data() + 48, "1.2.3", 6);
  std::memcpy(bytes.data() + 80, project, std::strlen(project));
  return bytes;
}

bool hasGuestMarker(const std::vector<uint8_t>& bytes, size_t chunk = 17) {
  guest_slot::Scanner scanner;
  for (size_t pos = 0; pos < bytes.size(); pos += chunk)
    scanner.feed(bytes.data() + pos, std::min(chunk, bytes.size() - pos));
  return scanner.exactlyOne();
}

}  // namespace

TEST(GuestDevLoop, FlagOpensInstallOnNormalBootOnly) {
  EXPECT_FALSE(guest_devloop::shouldOpen(false, false, false, false));
  EXPECT_TRUE(guest_devloop::shouldOpen(true, false, false, false));
  EXPECT_FALSE(guest_devloop::shouldOpen(true, true, false, false));
  EXPECT_FALSE(guest_devloop::shouldOpen(true, false, true, false));
  EXPECT_FALSE(guest_devloop::shouldOpen(true, false, false, true));
}

TEST(GuestSlot, DescriptorRejectsAbsentWrongProjectAndCorruption) {
  std::vector<uint8_t> absent(4096, 0xFF);
  EXPECT_FALSE(inspect(absent).installed);
  auto crosspoint = image("crosspoint-reader");
  EXPECT_FALSE(inspect(crosspoint).installed);
  auto wrong = image("readmaxer-xteink-x4");
  EXPECT_FALSE(inspect(wrong).installed);
  auto guest = image("readmaxer-xteink-x4pro");
  EXPECT_TRUE(inspect(guest).installed);
  EXPECT_STREQ(inspect(guest).version, "1.2.3");
  guest[32] ^= 1;
  EXPECT_FALSE(inspect(guest).installed);
}

TEST(GuestSlot, DescriptorRejectsOversizedAndTornImages) {
  auto guest = image("readmaxer-xteink-x4pro");
  uint32_t oversized = 4096;
  std::memcpy(guest.data() + 28, &oversized, 4);
  EXPECT_FALSE(inspect(guest).installed);
  guest = image("readmaxer-xteink-x4pro");
  guest.resize(64);
  EXPECT_FALSE(inspect(guest).installed);
  guest = image("readmaxer-xteink-x4pro");
  std::memset(guest.data() + 48, 'x', 32);
  EXPECT_FALSE(inspect(guest).installed);
}

TEST(GuestSlot, CountsMarkersAcrossChunksAndAfterPartialPrefixes) {
  const char marker[] = "READMAXER-GUEST-V1;";
  std::vector<uint8_t> one(marker, marker + sizeof(marker) - 1);
  EXPECT_TRUE(hasGuestMarker(one, 1));
  one.insert(one.begin(), {'R', 'E', 'A', 'D', 'M', 'A', 'X', 'E'});
  EXPECT_TRUE(hasGuestMarker(one, 3));
  one.insert(one.end(), marker, marker + sizeof(marker) - 1);
  EXPECT_FALSE(hasGuestMarker(one, 2));
  EXPECT_FALSE(hasGuestMarker(std::vector<uint8_t>{'R', 'E', 'A', 'D'}));
}

TEST(GuestSlot, HostMarkerRequiresExactlyOneOccurrence) {
  host_tag::Scanner scanner;
  EXPECT_EQ(host_tag::classify(true, false, scanner.count(), 0), host_tag::Identity::WrongFork);
  EXPECT_EQ(host_tag::classify(false, false, scanner.count(), 1), host_tag::Identity::Guest);
  const char* tag = host_tag::TAG;
  scanner.feed(reinterpret_cast<const uint8_t*>(tag), 3);
  scanner.feed(reinterpret_cast<const uint8_t*>(tag + 3), std::strlen(tag) - 3);
  EXPECT_TRUE(scanner.exactlyOne());
  EXPECT_EQ(host_tag::classify(true, false, scanner.count(), 0), host_tag::Identity::Host);
  EXPECT_NE(host_tag::classify(true, false, scanner.count(), 1), host_tag::Identity::Host);
  EXPECT_EQ(host_tag::classify(false, false, scanner.count(), 1), host_tag::Identity::WrongBoard);
  EXPECT_EQ(host_tag::classify(true, true, scanner.count(), 0), host_tag::Identity::WrongBoard);
  scanner.feed(reinterpret_cast<const uint8_t*>(tag), std::strlen(tag));
  EXPECT_FALSE(scanner.exactlyOne());
  EXPECT_EQ(host_tag::classify(true, false, scanner.count(), 0), host_tag::Identity::WrongFork);
}

TEST(GuestSlot, SdGateEnumeratesHostAndGuestMarkerCounts) {
  for (unsigned hosts : {0u, 1u, 2u}) {
    for (unsigned guests : {0u, 1u, 2u, 300u}) {
      host_tag::Scanner host;
      guest_slot::Scanner guest;
      for (unsigned i = 0; i < hosts; ++i)
        host.feed(reinterpret_cast<const uint8_t*>(host_tag::TAG), std::strlen(host_tag::TAG));
      const char marker[] = "READMAXER-GUEST-V1;";
      for (unsigned i = 0; i < guests; ++i)
        for (const char* c = marker; *c; ++c) guest.feed(reinterpret_cast<const uint8_t*>(c), 1);
      for (bool matched : {false, true}) {
        for (bool mismatch : {false, true}) {
          const auto identity = host_tag::classify(matched, mismatch, host.count(), guest.count());
          EXPECT_EQ(identity == host_tag::Identity::Host, matched && !mismatch && hosts == 1 && guests == 0)
              << "host=" << hosts << " guest=" << guests;
        }
      }
    }
  }
}

TEST(GuestSlot, OneHostAndTwoGuestMarkersNeverClassifyAsHost) {
  EXPECT_NE(host_tag::classify(true, false, 1, 2), host_tag::Identity::Host);
}

TEST(GuestSlot, OneHostAndOneGuestMarkerNeverClassifyAsHost) {
  EXPECT_NE(host_tag::classify(true, false, 1, 1), host_tag::Identity::Host);
}

TEST(GuestSlot, OptionalProbeImageIsNotGuest) {
  const char* path = std::getenv("CP2_PROBE_IMAGE");
  if (!path) GTEST_SKIP() << "CP2_PROBE_IMAGE is optional";
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file.good()) << path;
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  EXPECT_FALSE(inspect(bytes).installed && hasGuestMarker(bytes));
}

TEST(GuestSlot, OptionalForkImageIsNotGuest) {
  const char* path = std::getenv("CP2_FORK_IMAGE");
  if (!path) GTEST_SKIP() << "CP2_FORK_IMAGE is optional";
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file.good()) << path;
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  EXPECT_FALSE(inspect(bytes).installed && hasGuestMarker(bytes));
}
