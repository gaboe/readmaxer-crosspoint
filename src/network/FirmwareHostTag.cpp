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

Identity classify(bool boardMatched, bool boardMismatch, const Scanner& host, bool guestMarker) {
  if (boardMatched && !boardMismatch && host.exactlyOne() && !guestMarker) return Identity::Host;
  if (!host.exactlyOne() && guestMarker) return Identity::Guest;
  if (!boardMatched || boardMismatch) return Identity::WrongBoard;
  return Identity::WrongFork;
}

}  // namespace host_tag
