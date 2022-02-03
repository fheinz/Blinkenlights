/* Copyright 2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Utility functions for the blinkenlights board firmware.
 *
 */

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
 *
 * Returns true if successful, false if the substring contains
 * at least one non-decimal digit.
 */
bool ParseUInt32(uint32_t *i, const char *from);

/*
 * Parse a substring into an array of bytes using hexadecimal
 * representation.
 *
 * Parameters:
 *    buf:  pointer to byte array for the result;
 *          must be large enough to hold the bytes
 *    from: start of the string representation
 *
 * Returns true if successful, false if the substring contains
 * at least one non-hex digit.
 */
bool ParseHex(uint8_t *buf, const char *from);

}  // namespace util
}  // namespace blink

#endif  // BLINKENLIGHTS_UTIL_H_
