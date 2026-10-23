#include "FirmwareHostTag.h"

namespace host_tag {

const char TAG[] __attribute__((used)) =
    "READMAXER-"
    "HOST-V1;";

void Scanner::feed(const uint8_t* data, size_t len) {
  for (size_t n = 0; n < len; ++n) {
    const char c = static_cast<char>(data[n]);
    window[total % kLength] = c;
    ++total;
    if (c != ';' || total < kLength) continue;
    bool equal = true;
    for (size_t i = 0; i < kLength; ++i) {
      if (window[(total - kLength + i) % kLength] != TAG[i]) {
        equal = false;
        break;
      }
    }
    if (equal && occurrences < 2) ++occurrences;
  }
}

Identity classify(bool boardMatched, bool boardMismatch, uint8_t hostCount, uint8_t guestCount) {
  if (boardMatched && !boardMismatch && hostCount == 1 && guestCount == 0) return Identity::Host;
  if (hostCount == 0 && guestCount > 0) return Identity::Guest;
  if (!boardMatched || boardMismatch) return Identity::WrongBoard;
  return Identity::WrongFork;
}

}  // namespace host_tag
