#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "OtaSlotModel.h"

namespace {

bool readBytes(uint32_t offset, void* dst, uint32_t len, void* ctx) {
  const auto& bytes = *static_cast<const std::vector<uint8_t>*>(ctx);
  if (offset > bytes.size() || len > bytes.size() - offset) return false;
  std::memcpy(dst, bytes.data() + offset, len);
  return true;
}

ota_boot::GuestImage inspect(std::vector<uint8_t>& bytes) {
  uint8_t buf[17];
  return ota_boot::inspectGuestImage(readBytes, &bytes, bytes.size(), buf, sizeof(buf));
}

std::vector<uint8_t> image(const char* project, const char* marker = nullptr) {
  std::vector<uint8_t> bytes(32 + 256, 0);
  bytes[0] = 0xE9;
  bytes[1] = 1;
  const uint32_t length = 256;
  std::memcpy(bytes.data() + 28, &length, 4);
  const uint32_t magic = 0xABCD5432;
  std::memcpy(bytes.data() + 32, &magic, 4);
  std::memcpy(bytes.data() + 48, "1.2.3", 6);
  std::memcpy(bytes.data() + 80, project, std::strlen(project));
  if (marker) std::memcpy(bytes.data() + 32 + 160, marker, std::strlen(marker));
  return bytes;
}

}  // namespace

TEST(GuestSlot, AbsentAndWrongProject) {
  std::vector<uint8_t> absent(4096, 0xFF);
  EXPECT_FALSE(inspect(absent).installed);
  auto crosspoint = image("crosspoint-reader", "READMAXER-GUEST-V1;");
  EXPECT_FALSE(inspect(crosspoint).installed);
  auto wrong = image("readmaxer-xteink-x4", "READMAXER-GUEST-V1;");
  EXPECT_FALSE(inspect(wrong).installed);
}

TEST(GuestSlot, MarkerAndVersion) {
  auto guest = image("readmaxer-xteink-x4pro", "READMAXER-GUEST-V1;");
  const auto installed = inspect(guest);
  EXPECT_TRUE(installed.installed);
  EXPECT_STREQ(installed.version, "1.2.3");
  guest.insert(guest.end(), 20, 0);
  std::memcpy(guest.data() + 32 + 200, "READMAXER-GUEST-V1;", 19);
  EXPECT_FALSE(inspect(guest).installed);
  guest[32] ^= 1;
  EXPECT_FALSE(inspect(guest).installed);
}

TEST(GuestSlot, ExistingImagesHaveNoGuestMarker) {
  for (const char* key : {"CP2_PROBE_IMAGE", "CP2_FORK_IMAGE"}) {
    const char* path = std::getenv(key);
    ASSERT_NE(path, nullptr) << key;
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.good()) << path;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    EXPECT_FALSE(inspect(bytes).installed) << path;
  }
}
