#include "util.h"

#include <cctype>

namespace blink {
namespace util {

bool ParseUInt32(uint32_t *i, const char *from, const char *to) {
  uint32_t acc = 0;
  while (from < to) {
    char c = *from++;
    if (!isdigit(c)) return false;
    acc = acc * 10 + (c - '0');
  }
  *i = acc;
  return true;
}

bool ParseHex(uint8_t *buf, const char *from, const char *to) {
  bool half = false;
  while (from < to) {
    char c = *from++;
    uint8_t d;
    if (isdigit(c)) {
      d = c - '0';
    } else if (isxdigit(c)) {
      d = (c + 10 - 'A');
    } else {
      return false;
    }
    if (half) {
      *buf |= d;
      buf++;
    } else {
      *buf = d << 4;
    }
    half = !half;
  }
  return true;
}

}  // namespace util
}  // namespace blink