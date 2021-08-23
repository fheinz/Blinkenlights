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
 * Blinkenlights board firmware animation logic.
 *
 */

#ifndef BLINKENLIGHTS_ANIMATOR_H_
#define BLINKENLIGHTS_ANIMATOR_H_

#include <Stream.h>

#include <cstdint>
#include <functional>

#include "frame.h"
#include "util.h"

namespace blink {
namespace animation {

template <size_t kMaxAnimations, size_t kMaxFrames, size_t kWidth,
          size_t kHeight>
class Animatior {
 public:
  using NowFunc = std::function<unsigned long int()>;

  explicit Animatior(NowFunc now)
      : now_(now),
        frames_(),
        sentinel_frame_(),
        animations_(),
        frames_start_idx_(0),
        frames_length_(0),
        animation_start_idx_(0),
        animation_length_(0) {
    sentinel_frame_.Clear();
  }

  bool CanLoadAnimation() const {
    return animation_length_ < kMaxAnimations && CanLoadFrame();
  }

  bool StartLoadingAnimation(uint32_t duration_millis) {
    if (!CanLoadAnimation()) {
      return false;
    }
    if (animation_length_ > 0) {
      FinalizeLoadingAnimation();
    }
    size_t idx = (animation_start_idx_ + animation_length_) % kMaxAnimations;
    animations_[idx].being_loaded = true;
    animations_[idx].started = false;
    size_t frame_idx = (frames_start_idx_ + frames_length_) % kMaxFrames;
    animations_[idx].frame_start_idx = frame_idx;
    animations_[idx].num_frames = 0;
    animations_[idx].duration = duration_millis;
    animation_length_++;
    return true;
  }

  bool IsLoadingAnimation() const {
    if (animation_length_ == 0) {
      return 0;
    }
    size_t idx =
        (animation_start_idx_ + animation_length_ - 1) % kMaxAnimations;
    return animations_[idx].being_loaded;
  }

  void FinalizeLoadingAnimation() {
    if (animation_length_ == 0) {
      return;
    }
    size_t idx =
        (animation_start_idx_ + animation_length_ - 1) % kMaxAnimations;
    animations_[idx].being_loaded = false;
  }

  bool CanLoadFrame() const { return frames_length_ < kMaxFrames; }

  bool GetFrameToLoad(Frame<kWidth, kHeight>** frame_out) {
    if (!CanLoadFrame()) {
      frame_out = nullptr;
      return false;
    }
    size_t idx = (frames_start_idx_ + frames_length_) % kMaxFrames;
    frames_length_++;
    frames_[idx].Rewrite();
    *frame_out = &frames_[idx];
    size_t animation_idx =
        (animation_start_idx_ + animation_length_ - 1) % kMaxAnimations;
    animations_[animation_idx].num_frames++;
    return true;
  }

  // Main function that will check time and return the frame that should
  // be displayed. If all animations are consumed, it will return a blank
  // sentinel frame.
  const Frame<kWidth, kHeight>& GetCurrentFrame() {
    uint32_t curr_time = now_();

    // Has the current animation expired?
    if (animation_length_ > 0 && animations_[animation_start_idx_].started &&
        curr_time >= animation_expiration_) {
      frames_start_idx_ =
          (frames_start_idx_ + animations_[animation_start_idx_].num_frames) %
          kMaxFrames;
      frames_length_ -= animations_[animation_start_idx_].num_frames;
      animation_start_idx_ = (animation_start_idx_ + 1) % kMaxAnimations;
      animation_length_--;
    }

    // Discard frameless animations that are fully loaded.
    while (animation_length_ > 0 &&
           animations_[animation_start_idx_].num_frames == 0 &&
           !animations_[animation_start_idx_].being_loaded) {
      animation_start_idx_ = (animation_start_idx_ + 1) % kMaxAnimations;
      animation_length_--;
    }

    // Show the sentinel frame if there's nothing better.
    if (animation_length_ == 0 ||
        animations_[animation_start_idx_].being_loaded) {
      return sentinel_frame_;
    }

    // Make sure we have a scheduled animation.
    if (!animations_[animation_start_idx_].started) {
      animations_[animation_start_idx_].started = true;
      animation_expiration_ =
          curr_time + animations_[animation_start_idx_].duration;
      curr_frame_ = animations_[animation_start_idx_].frame_start_idx;
      frame_expiration_ = curr_time + frames_[curr_frame_].GetDuration();
    }

    // Has the current frame expired?
    if (curr_time >= frame_expiration_) {
      curr_frame_ = (curr_frame_ + 1) % kMaxFrames;
      size_t end = (animations_[animation_start_idx_].frame_start_idx +
                    animations_[animation_start_idx_].num_frames) %
                   kMaxFrames;
      if (curr_frame_ == end) {
        curr_frame_ = animations_[animation_start_idx_].frame_start_idx;
      }
      frame_expiration_ = curr_time + frames_[curr_frame_].GetDuration();
    }

    return frames_[curr_frame_];
  }

  size_t GetNumFreeFrameSlots() const { return kMaxFrames - frames_length_; }

  size_t GetNumFreeAnimationSlots() const {
    return kMaxAnimations - animation_length_;
  }

  void SkipCurrentAnimation() {
    if (animation_length_ < 2) {
      return;
    }
    frames_start_idx_ =
        (frames_start_idx_ + animations_[animation_start_idx_].num_frames) %
        kMaxFrames;
    frames_length_ -= animations_[animation_start_idx_].num_frames;
    animation_start_idx_ = (animation_start_idx_ + 1) % kMaxAnimations;
    animation_length_--;
  }

  void Reset() {
    frames_start_idx_ = 0;
    frames_length_ = 0;
    animation_start_idx_ = 0;
    animation_length_ = 0;
    curr_frame_ = 0;
  }

  void DebugDumpln(Stream& stream) const {
    stream.print("Animations: start=");
    stream.print(animation_start_idx_);
    stream.print(" len=");
    stream.print(animation_length_);
    stream.print(" total=");
    stream.println(kMaxAnimations);
    stream.print("Frames: start=");
    stream.print(frames_start_idx_);
    stream.print(" len=");
    stream.print(frames_length_);
    stream.print(" total=");
    stream.print(kMaxFrames);
    stream.print(" current=");
    stream.println(curr_frame_);
    for (int i = 0; i < kMaxAnimations; ++i) {
      stream.print(i);
      stream.print(" ");
      animations_[i].DebugDumpln(stream);
    }
    for (int i = 0; i < kMaxFrames; ++i) {
      stream.print(i);
      stream.print(" ");
      frames_[i].DebugDumpln(stream);
    }
  }

 private:
  // Animation covers a range of frames in the frame array together with overall
  // animation duration.
  struct Animation {
    bool being_loaded;
    bool started;
    size_t frame_start_idx;
    size_t num_frames;
    uint32_t duration;  // millis

    void DebugDumpln(Stream& stream) const {
      stream.print("Animation { ");
      stream.print(being_loaded);
      stream.print(", ");
      stream.print(started);
      stream.print(", ");
      stream.print(frame_start_idx);
      stream.print(", ");
      stream.print(num_frames);
      stream.print(", ");
      stream.print(duration);
      stream.println(" }");
    }
  };

  // Returns current board time in millis.
  NowFunc now_;
  // Array of frames, not necessarily loaded. Animations own continuous chunks
  // of these, no spaces between. Think of it as a circular buffer, where new
  // frames are loaded and consumed in animations.
  Frame<kWidth, kHeight> frames_[kMaxFrames];
  // A frame to display when there are no loaded animations or all of them have
  // expired.
  Frame<kWidth, kHeight> sentinel_frame_;
  // Array of animations, not necessarily loaded. Think of it as a circular
  // buffer, where new animations are loaded and consumed when displayed.
  Animation animations_[kMaxAnimations];
  // Circular buffer start and length for frames and animations.
  size_t frames_start_idx_;
  size_t frames_length_;
  size_t animation_start_idx_;
  size_t animation_length_;

  // When the current animation expires (millis).
  uint32_t animation_expiration_;
  // Index of the current frame.
  size_t curr_frame_;
  // When the current frame expires (millis).
  uint32_t frame_expiration_;
};

}  // namespace animation
}  // namespace blink

#endif  // BLINKENLIGHTS_ANIMATOR_H_
