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
 * Blinkenlights board firmware frame definition.
 *
 */

#ifndef BLINKENLIGHTS_FRAME_H_
#define BLINKENLIGHTS_FRAME_H_

#include <FastLED.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <type_traits>
#include <utility>

#include "util.h"

namespace blink {
namespace animation {

template <size_t kWidth, size_t kHeight>
class Frame {
 public:
  // Pixel setting function used to populate the
  using PixelSetter = std::function<void(size_t y, size_t x, const CRGB& rgb)>;

  Frame() : pixels_(), load_next_idx_(0), duration_millis_(0) {}

  // From raw pixels array as char arguments.
  // TODO: Make this work with std::initializer_list instead.
  template <typename... Pixels,
            typename = typename std::enable_if<std::is_same<
                typename std::common_type<Pixels...>::type, char>::value>::type>
  constexpr explicit Frame(Pixels&&... pixels)
      : pixels_{std::forward<Pixels>(pixels)...},
        load_next_idx_(0),
        duration_millis_(0) {}

  // Reset the internal loading counters.
  void StartLoading() { load_next_idx_ = 0; }

  // Assumes that a single pixel is represented as ASCII HEX byte triplet of the
  // the following form RRGGGBB, e.g. 0AFF08 for RGB(10, 255, 8). Then the
  // buffer is a plain concatination of these, no spaces.
  // Stops when the frame is loaded and returns true, even if it hasn't consumed
  // the whole buffer.
  // Returns false if the buffer can't be parsed.
  bool LoadPartFromAsciiHexBuffer(const char* buffer) {
    size_t max_to_load = kNumBytes - load_next_idx_;
    size_t want_to_load = strlen(buffer) / 2;
    size_t to_load = want_to_load <= max_to_load ? want_to_load : max_to_load;
    if (!blink::util::ParseHex(pixels_ + load_next_idx_, buffer)) {
      return false;
    }
    load_next_idx_ += to_load;  // Two hex digits make a byte.
    return true;
  }

  size_t RowBeingLoaded() const { return load_next_idx_ / 3 / kWidth; }

  // True if the whole frame has been loaded since the last call to
  // StartLoading.
  bool IsDone() const { return load_next_idx_ >= kNumBytes; }

  // Assumes that the display size matches frame size.
  void CopyToFastLedDisplay(PixelSetter pixel_setter) const {
    size_t idx = 0;
    for (int y = 0; y < kHeight; y++) {
      for (int x = 0; x < kWidth; x++) {
        pixel_setter(y, x,
                     CRGB(pixels_[idx], pixels_[idx + 1], pixels_[idx + 2]));
        idx += 3;
      }
    }
  }

  void SetDuration(uint32_t duration_millis) {
    duration_millis_ = duration_millis;
  }

  uint32_t GetDuration() const { return duration_millis_; }

  void Clear() { memset(pixels_, 0, kNumBytes); }
  void Rewrite() { load_next_idx_ = 0; }

  void SetPixel(int y, int x, uint8_t r, uint8_t g, uint8_t b) {
    int idx = y * kWidth * 3 + x * 3;
    pixels_[idx] = r;
    pixels_[idx + 1] = g;
    pixels_[idx + 2] = b;
  }

  void DebugDumpln(Stream& stream) const {
    stream.print(F("Frame { <lots of pixels>, "));
    stream.print(load_next_idx_);
    stream.print(F(", "));
    stream.print(duration_millis_);
    stream.println(F(" }"));
  }

 private:
  static constexpr size_t kNumPixels = kWidth * kHeight;
  static constexpr size_t kNumBytes = kNumPixels * 3;

  // Flattened representation of a frame where each pixel is represented as 3
  // bytes, R, G and B respectively in the top-to-bottom left-to-right order
  // of pixels.
  uint8_t pixels_[kNumBytes];
  // Index of the next pixels_ byte to load.
  size_t load_next_idx_;
  // Frame duration in milliseconds.
  uint32_t duration_millis_;
};

}  // namespace animation
}  // namespace blink

#endif  // BLINKENLIGHTS_FRAME_H_
