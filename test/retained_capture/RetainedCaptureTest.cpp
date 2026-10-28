#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <string>

#include "src/network/RetainedCapture.h"

namespace {
using guest_log::RetainedCapture;
using Bytes = std::array<uint8_t, RetainedCapture::BYTES>;

uint32_t crc32(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffff;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return ~crc;
}

void put(uint8_t* bytes, uint64_t value, size_t width) {
  for (size_t i = 0; i < width; ++i) bytes[i] = value >> (i * 8);
}

void seal(uint8_t* bytes, size_t size) { put(bytes + size - 8, crc32(bytes, size - 8), 4); }

Bytes fixture() {
  std::ifstream input(RTC_FIXTURE);
  std::string hex, line;
  while (input >> line) hex += line;
  EXPECT_EQ(hex.size(), RetainedCapture::BYTES * 2);
  Bytes bytes{};
  if (hex.size() != bytes.size() * 2) return bytes;
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = std::stoul(hex.substr(i * 2, 2), nullptr, 16);
  return bytes;
}

TEST(RetainedCapture, RustWriterDecodesWithoutTruncatingTimeOrRenumbering) {
  auto bytes = fixture();
  RetainedCapture capture;
  capture.capture(bytes.data());
  ASSERT_EQ(capture.format(), RetainedCapture::Format::Tail);
  ASSERT_EQ(capture.eventCount(), 9u);
  ASSERT_NE(capture.emergency(0), nullptr);
  ASSERT_NE(capture.emergency(1), nullptr);
  EXPECT_EQ(capture.newestEmergency(), capture.emergency(1));
  EXPECT_EQ(capture.header()[16], 7);
  for (size_t i = 0; i < 9; ++i) {
    const auto event = capture.event(i);
    EXPECT_EQ(event.sequence, 41 + i);
    EXPECT_EQ(event.timeMs, uint64_t(UINT32_MAX) + 108 - i);
    ASSERT_EQ(event.length, 13);
    EXPECT_EQ(event.payload[0], 2);
    EXPECT_EQ(event.payload[1], 30);
    EXPECT_EQ(event.payload[2], 9 - i);
  }
  EXPECT_EQ(capture.emergency(1)[304 + 1], 49);
  EXPECT_EQ(capture.emergency(1)[224], 40);
  EXPECT_EQ(capture.emergency(1)[233], 48);
  EXPECT_EQ(capture.emergency(1)[232], 0);
  EXPECT_EQ(capture.event(9).payload, nullptr);
}

TEST(RetainedCapture, LegacyVersionsAndUnknownVersionsRemainDistinct) {
  auto bytes = fixture();
  RetainedCapture capture;
  bytes[4] = 1;
  std::fill(bytes.begin() + 64, bytes.begin() + 504, 0);
  seal(bytes.data(), 512);
  const std::string text = "SD failure code=-17\n";
  std::fill(bytes.begin() + 1024, bytes.end(), 0);
  std::copy_n("RMLG", 4, bytes.begin() + 1024);
  put(bytes.data() + 1028, 1, 2);
  put(bytes.data() + 1030, text.size(), 2);
  put(bytes.data() + 1032, crc32(reinterpret_cast<const uint8_t*>(text.data()), text.size()), 4);
  std::copy(text.begin(), text.end(), bytes.begin() + 1040);
  capture.capture(bytes.data());
  ASSERT_NE(capture.emergency(0), nullptr);
  ASSERT_EQ(capture.emergency(0)[4], 1);
  ASSERT_EQ(capture.format(), RetainedCapture::Format::LegacyText);
  EXPECT_EQ(std::string(capture.legacyText(), capture.legacyLength()), text);
  bytes[4] = 9;
  seal(bytes.data(), 512);
  put(bytes.data() + 1028, 9, 2);
  capture.capture(bytes.data());
  EXPECT_EQ(capture.emergency(0), nullptr);
  EXPECT_EQ(capture.format(), RetainedCapture::Format::Unsupported);
  EXPECT_EQ(capture.eventCount(), 0u);
}

TEST(RetainedCapture, CorruptAndStaleCellsDoNotHideOtherEvents) {
  auto bytes = fixture();
  RetainedCapture capture;
  bytes[1152 + 24] ^= 1;
  put(bytes.data() + 1152 + 96 + 16, 6, 4);
  seal(bytes.data() + 1152 + 96, 96);
  capture.capture(bytes.data());
  EXPECT_EQ(capture.eventCount(), 7u);
  EXPECT_EQ(capture.event(0).sequence, 41u);
  EXPECT_EQ(capture.event(6).sequence, 47u);
  bytes[1024 + 64 + 56] ^= 1;
  capture.capture(bytes.data());
  EXPECT_EQ(capture.header()[16], 6);
  EXPECT_EQ(capture.eventCount(), 1u);
}

TEST(RetainedCapture, HostResetPreservesGuestFailureAndNeverChangesRtc) {
  auto bytes = fixture();
  const auto original = bytes;
  RetainedCapture first;
  first.capture(bytes.data());
  // Host RAM can disappear before archive or acknowledgement. RTC remains the source.
  RetainedCapture afterReset;
  afterReset.capture(bytes.data());
  EXPECT_EQ(bytes, original);
  EXPECT_EQ(first.raw(), afterReset.raw());
  ASSERT_NE(afterReset.newestEmergency(), nullptr);
  EXPECT_EQ(afterReset.newestEmergency()[224], 40);
  EXPECT_EQ(afterReset.newestEmergency()[305], 49);
  EXPECT_EQ(afterReset.eventCount(), 9u);
  std::fill(bytes.begin(), bytes.end(), 0);
  EXPECT_EQ(first.raw(), original);
}

TEST(RetainedCapture, EveryHeaderCellCrcAndCommitStoreCutRejectsTornReplacement) {
  const auto old = fixture();
  for (auto layout : {std::array<size_t, 2>{1088, 64}, {1152, 96}, {512, 512}}) {
    const size_t offset = layout[0], size = layout[1];
    auto replacement = old;
    const size_t change = size == 64 ? 8 : 0;
    replacement[offset + change] ^= 0x10;
    if (size == 512) {
      replacement[offset] = 'R';
      replacement[offset + 8] = 9;
    }
    seal(replacement.data() + offset, size);
    for (size_t cut = 0; cut <= size - 2; ++cut) {
      auto bytes = old;
      if (cut > 0) std::fill_n(bytes.begin() + offset + size - 4, 4, 0);
      const size_t body = cut > 1 ? std::min(cut - 1, size - 4) : 0;
      std::copy_n(replacement.begin() + offset, body, bytes.begin() + offset);
      if (cut == size - 2) std::copy_n(replacement.begin() + offset + size - 4, 4, bytes.begin() + offset + size - 4);
      RetainedCapture capture;
      capture.capture(bytes.data());
      if (cut == 0 || cut == size - 2) continue;
      if (size == 64) {
        ASSERT_NE(capture.header(), nullptr);
        EXPECT_EQ(capture.header()[16], 6);
      }
      if (size == 96) EXPECT_EQ(capture.eventCount(), 8u);
      if (size == 512) {
        EXPECT_EQ(capture.emergency(1), nullptr);
        ASSERT_NE(capture.emergency(0), nullptr);
      }
    }
  }
}
}  // namespace
