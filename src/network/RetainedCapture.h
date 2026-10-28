#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace guest_log {

class RetainedCapture {
 public:
  static constexpr uintptr_t ADDRESS = 0x50001800;
  static constexpr size_t BYTES = 2048;
  static constexpr size_t CELL_COUNT = 9;
  enum class Format { Unavailable, LegacyText, Tail, Unsupported };

  struct Event {
    uint64_t sequence = 0;
    uint64_t timeMs = 0;
    const uint8_t* payload = nullptr;
    uint8_t length = 0;
  };

  void capture(const volatile uint8_t* source);
  Format format() const { return savedFormat; }
  const std::array<uint8_t, BYTES>& raw() const { return bytes; }
  const uint8_t* emergency(size_t index) const;
  const uint8_t* newestEmergency() const;
  const uint8_t* header() const;
  size_t eventCount() const { return count; }
  Event event(size_t index) const;
  const char* legacyText() const;
  size_t legacyLength() const { return textLength; }

 private:
  std::array<uint8_t, BYTES> bytes{};
  std::array<uint16_t, CELL_COUNT> cells{};
  std::array<bool, 2> validEmergency{};
  Format savedFormat = Format::Unavailable;
  size_t count = 0;
  size_t textLength = 0;
  int headerIndex = -1;
};

static_assert(sizeof(RetainedCapture) < 2304);

}  // namespace guest_log
