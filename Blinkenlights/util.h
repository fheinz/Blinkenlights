#ifndef BLINKENLIGHTS_UTIL_H_
#define BLINKENLIGHTS_UTIL_H_

#include <cstdint>

namespace blink {
namespace util {

/*
 * Parse a substring into an unsigned int 32 using decimal
 * representation.
 *
 * Parameters:
 *    i:    pointer to unsigned int for the result
 *    from: start of the string representation
 *    to:   end of the string representation
 *
 * Returns true if successful, false if the substring contains
 * at least one non-decimal digit.
 */
bool ParseUInt32(uint32_t *i, const char *from, const char *to);

/*
 * Parse a substring into an array of bytes using hexadecimal
 * representation.
 *
 * Parameters:
 *    buf:  pointer to byte array for the result;
 *          must be large enough to hold the bytes
 *    from: start of the string representation
 *    to:   end of the string representation
 *
 * Returns true if successful, false if the substring contains
 * at least one non-hex digit.
 */
bool ParseHex(uint8_t *buf, const char *from, const char *to);

}  // namespace util
}  // namespace blink

#endif  // BLINKENLIGHTS_UTIL_H_
