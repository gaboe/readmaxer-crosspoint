#include <CpBootRecord/CpBootRecord.h>
#include <gtest/gtest.h>

using namespace cp_boot;

TEST(CpBootRecord, EncodesAndDecodesBootAndFourAttempts) {
  Record source;
  source.begin(7);
  for (size_t i = 0; i < Record::Attempts; ++i) source.attempt(i, Stage::CardInit, 0x107 + i);
  source.attempt(3, Stage::SectorZero, -1);
  source.stage(Stage::PanelInit);
  Record decoded;
  ASSERT_TRUE(decoded.decode(source.encode()));
  ASSERT_EQ(decoded.count(), 1);
  EXPECT_EQ(decoded.counter(0), 1);
  EXPECT_EQ(decoded.resetReason(0), 7);
  EXPECT_EQ(decoded.lastStage(0), Stage::PanelInit);
  EXPECT_EQ(decoded.sdAttempt(0, 0).error, 0x107);
  EXPECT_EQ(decoded.sdAttempt(0, 2).error, 0x109);
  EXPECT_EQ(decoded.sdAttempt(0, 3).stage, Stage::SectorZero);
  EXPECT_EQ(decoded.sdAttempt(0, 3).error, -1);
  EXPECT_EQ(source.encode()[8], 1);
  EXPECT_EQ(source.encode()[9], 0);
}

TEST(CpBootRecord, RejectsEveryCorruptedByteAndRestartsHistory) {
  Record source;
  source.begin(3);
  for (size_t i = 0; i < Record::Bytes; ++i) {
    auto bytes = source.encode();
    bytes[i] ^= 1;
    Record decoded;
    EXPECT_FALSE(decoded.decode(bytes)) << i;
    EXPECT_EQ(decoded.count(), 0);
    decoded.begin(4);
    EXPECT_EQ(decoded.counter(0), 1);
  }
}

TEST(CpBootRecord, KeepsTwoNewestBootsWithPreviousFailure) {
  Record record;
  record.begin(1);
  record.begin(2);
  record.attempt(0, Stage::CardInit, Pending);
  record.begin(3);
  ASSERT_EQ(record.count(), 2);
  EXPECT_EQ(record.counter(0), 3);
  EXPECT_EQ(record.counter(1), 2);
  EXPECT_EQ(record.resetReason(1), 2);
  EXPECT_EQ(record.lastStage(1), Stage::CardInit);
  EXPECT_EQ(record.sdAttempt(1, 0).error, Pending);
  EXPECT_EQ(record.sdAttempt(0, 0).stage, Stage::None);
}

TEST(CpBootRecord, CounterWrapAndErrorRangeAreExplicit) {
  Record record;
  for (uint32_t i = 0; i < 65536; ++i) record.begin(1);
  EXPECT_EQ(record.counter(0), 0);
  EXPECT_EQ(record.counter(1), 65535);
  record.attempt(0, Stage::HostInit, INT32_MAX);
  EXPECT_EQ(record.sdAttempt(0, 0).error, ErrorOutOfRange);
  record.attempt(0, Stage::HostInit, INT32_MIN);
  EXPECT_EQ(record.sdAttempt(0, 0).error, ErrorOutOfRange);
  record.attempt(0, Stage::HostInit, Pending);
  EXPECT_EQ(record.sdAttempt(0, 0).error, Pending);
}

TEST(CpBootRecord, MatchesVersionOneWireFixture) {
  // CRC fixture computed with Python zlib.crc32, independently of Record.
  const Record::Encoded fixture = {0x43, 0x50, 0x42, 0x54, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x07,
                                   0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa3, 0x6f, 0x84, 0x02};
  Record record;
  record.begin(7);
  EXPECT_EQ(record.encode(), fixture);
  Record decoded;
  ASSERT_TRUE(decoded.decode(fixture));
  EXPECT_EQ(decoded.counter(0), 1);
  EXPECT_EQ(decoded.lastStage(0), Stage::SetupEnter);
}
