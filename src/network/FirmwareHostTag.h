#pragma once

#include <cstddef>
#include <cstdint>

namespace host_tag {

extern const char TAG[];

class Scanner {
 public:
  void feed(const uint8_t* data, size_t len);
  uint8_t count() const { return occurrences; }
  bool exactlyOne() const { return occurrences == 1; }

 private:
  static constexpr size_t kLength = sizeof(
                                        "READMAXER-"
                                        "HOST-V1;") -
                                    1;
  char window[kLength]{};
  size_t total = 0;
  uint8_t occurrences = 0;
};

enum class Identity { Host, Guest, WrongBoard, WrongFork };
Identity classify(bool boardMatched, bool boardMismatch, uint8_t hostCount, uint8_t guestCount);

}  // namespace host_tag
