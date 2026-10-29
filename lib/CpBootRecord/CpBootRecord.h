#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace cp_boot {
enum class Stage : uint8_t {
  None,
  SetupEnter,
  StorageBegin,
  HostInit,
  SlotInit,
  CardInit,
  SectorZero,
  VolumeMount,
  StorageEnd,
  StorageFailed,
  PanelInit,
  FirstDraw,
};

const char* stageName(Stage stage);
constexpr int16_t Pending = INT16_MIN;
constexpr int16_t ErrorOutOfRange = INT16_MIN + 1;

struct Attempt {
  Stage stage;
  int16_t error;
};

// Wire format is little endian. No packed or unaligned integer accesses.
class Record {
 public:
  static constexpr size_t Bytes = 44;
  static constexpr size_t Boots = 2;
  static constexpr size_t Attempts = 4;
  using Encoded = std::array<uint8_t, Bytes>;

  bool decode(const Encoded& source);
  const Encoded& encode() const { return bytes; }
  void begin(uint8_t resetReason);
  void stage(Stage stage);
  void attempt(size_t index, Stage stage, int32_t error);
  size_t count() const { return bytes[5]; }
  uint16_t counter(size_t boot) const;
  uint8_t resetReason(size_t boot) const;
  Stage lastStage(size_t boot) const;
  Attempt sdAttempt(size_t boot, size_t index) const;

 private:
  Encoded bytes{};
  void seal();
};

// Existing X4 Pro layout ends at 0x500017bc; guest RTC starts at 0x50001800.
static_assert(sizeof(Record) == Record::Bytes);
static_assert(Record::Bytes <= 0x50001800 - 0x500017bc);
}  // namespace cp_boot
