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
 * RGB LEDs Matrix Animation Frame
 *
 */

#include <BluetoothSerial.h>
#include <FastLED.h>
#include <Preferences.h>
#include <Stream.h>

#include "animator.h"
#include "frame.h"
#include "util.h"

constexpr int BaudRate = 115200;
constexpr int SerialRxBufferSize = 4 * 1024;

// Pin definitions
constexpr int kOnboardLed0Pin = 32;
constexpr int kOnboardLed1Pin = 33;
constexpr int kLedMatrixPowerPin0 = 12;
constexpr int kLedMatrixPowerPin1 = 26;
constexpr int kLedMatrixDataPin = 27;
constexpr int kUsbCc1Pin = 36;
constexpr int kUsbCc2Pin = 39;
constexpr int kTouch0Pin = 15;
constexpr int kTouch1Pin = 2;
constexpr int kTouch2Pin = 4;

constexpr int kPowerLedPin = kOnboardLed0Pin;
constexpr int kPowerLedPwmChannel = 0;
constexpr int kPowerLedPwmFrequency = 4000;
constexpr int kPowerLedPwmResolution = 8;
constexpr int kFullPowerLedBrightness = 255;
constexpr int kMaxPowerLedBrightness = 3 * kFullPowerLedBrightness / 4;
constexpr int kMinPowerLedBrightness = kFullPowerLedBrightness / 4;
constexpr int kPowerLedBreathDuration = 3000;
constexpr float kPowerLedBreathBeta = 0.5;
constexpr float kPowerLedBreathGamma = 0.14;

Preferences preferences;

enum class UsbCurrentAvailable {
  // Current available unknown
  kNone,

  // USB-C host and we can draw 3A.
  k3A,

  // USB-C host and we can draw 1.5A.
  k1_5A,

  // We have either a USB-C host with no high current
  // capability or a legacy host / 1.5A (BCS) power supply. We do
  // not have the hardware to tell them apart, so we
  // can't make any assumptions on current available
  // beyond 100mA (though most hosts will be fine with 500mA).
  kUsbStd,
};

// LED Matrix dimensions
constexpr int kLedMatrixNumCols = 16;
constexpr int kLedMatrixNumLines = 16;
constexpr int kLedMatrixNumLeds = kLedMatrixNumCols * kLedMatrixNumLines;
constexpr float kMatrixMaxCurrent =
    kLedMatrixNumLeds * 0.06f;           // WS2812B: 60mA/LED
constexpr float kMaxIdleCurrent = 0.5f;  // Matrix + ESP32 idle

/*
 * Called on violation of invariants. May be used for desperate
 * recovery attempts or error status signalling.
 * State is undefined at beginning of the call.
 *
 * Parameters:
 *    err:  informational error code
 *    from: start of the string representation
 *    to:   end of the string representation
 */

enum class ErrorCode {
  kInvalidFrameIndex,
  kEnqueueInvalidAnimation,
  kAddFrameToInvalidAnimation,
  kAddInvalidFrameToAnimation,
};
void CantHappen(const ErrorCode err) {}

/*
 * Read voltage at an input pin.
 *
 * By default we have 11db (1/3.6) attenuation with 1.1V reference,
 * so full scale voltage range is 1.1*3.6 = 3.96. In reality it's
 * clamped to Vdd (3.3V) so we will never get a reading above that,
 * but we need to the calculations using 3.96.
 */
float AnalogReadV(int pin) { return analogRead(pin) * 3.96f / 4096; }

/*
 * Detect USB-C current advertisement according to Universal
 * Serial Bus Type-C Cable and Connector Specification
 * Depending on cable orientation, either CC1 or CC2 will be >0V,
 * and that tells us how much current we can draw.
 * Voltage thresholds from Table 4-36:
 * >0.2V = connected, >0.66V = 1.5A, >1.23V = 3A.
 */
UsbCurrentAvailable PowerOverride = UsbCurrentAvailable::kNone;
constexpr char PowerOverridePrefsKey[] = "PowerOverride";

void SetPowerOverride(UsbCurrentAvailable c) {
  PowerOverride = c;
  preferences.putUInt(PowerOverridePrefsKey, (uint8_t)c);
}

void ResetPowerOverride() {
  preferences.remove(PowerOverridePrefsKey);
  PowerOverride = UsbCurrentAvailable::kNone;
}

void GetPowerOverride() {
  PowerOverride = (UsbCurrentAvailable)preferences.getUInt(
      PowerOverridePrefsKey, (uint8_t)UsbCurrentAvailable::kNone);
}

UsbCurrentAvailable DetermineMaxCurrent() {
  if (PowerOverride != UsbCurrentAvailable::kNone) return PowerOverride;
  float cc1 = AnalogReadV(kUsbCc1Pin);
  float cc2 = AnalogReadV(kUsbCc2Pin);
  float cc = max(cc1, cc2);
  if (cc > 1.23f) {
    return UsbCurrentAvailable::k3A;
  } else if (cc > 0.66f) {
    return UsbCurrentAvailable::k1_5A;
  } else {
    return UsbCurrentAvailable::kUsbStd;
  }
}

/*
 * Feed power to the LED matrix
 */
void EnableLEDPower() {
  // Make sure data pin is low so we don't latch up the LEDs.
  digitalWrite(kLedMatrixDataPin, LOW);

  // Enable 1.5A current to charge up the capacitances.
  digitalWrite(kLedMatrixPowerPin0, HIGH);

  delay(50);

  // Enable the second 1.5A switch to reduce switch resistance
  // even if we only have 1.5A total, because we can limit it in
  // firmware instead.
  digitalWrite(kLedMatrixPowerPin1, HIGH);
}

void DisableLEDPower() {
  digitalWrite(kLedMatrixDataPin, LOW);
  digitalWrite(kLedMatrixPowerPin1, LOW);
  digitalWrite(kLedMatrixPowerPin0, LOW);
}

UsbCurrentAvailable currentAvailable = UsbCurrentAvailable::kNone;
UsbCurrentAvailable currentAvailableDetected = UsbCurrentAvailable::kNone;

uint32_t PowerUpdate() {
  uint32_t nextLoop;
  UsbCurrentAvailable current_advertisement = DetermineMaxCurrent();
  if (currentAvailableDetected == current_advertisement) {
    // We detected this change 15ms ago, so it can't be a PD message
    // because those take 10ms max.
    switch (current_advertisement) {
      case UsbCurrentAvailable::k3A:
        FastLED.setBrightness(255 * (3.0f - kMaxIdleCurrent) /
                              kMatrixMaxCurrent);
        EnableLEDPower();
        break;
      case UsbCurrentAvailable::k1_5A:
        FastLED.setBrightness(255 * (1.5f - kMaxIdleCurrent) /
                              kMatrixMaxCurrent);
        EnableLEDPower();
        break;
      default:
        DisableLEDPower();
    }
    currentAvailable = current_advertisement;
  }
  if (currentAvailable == current_advertisement) {
    currentAvailableDetected = UsbCurrentAvailable::kNone;
    nextLoop = 30;
  } else {
    // We just detected this change... check back in 15ms to see if it's still
    // there.
    currentAvailableDetected = current_advertisement;
    nextLoop = 15;
  }
  if (currentAvailable == UsbCurrentAvailable::k3A) {
    ledcWrite(kPowerLedPwmChannel, kMaxPowerLedBrightness);
  } else if (currentAvailable == UsbCurrentAvailable::k1_5A) {
    float phase = static_cast<float>(millis() % kPowerLedBreathDuration) /
                  kPowerLedBreathDuration;
    float sqrt_numerator = (phase - kPowerLedBreathBeta) / kPowerLedBreathGamma;
    float ln_brightness = -sqrt_numerator * sqrt_numerator / 2.0;
    float gauss = exp(ln_brightness);
    int brightness = kMinPowerLedBrightness +
                     static_cast<int>(gauss * (kMaxPowerLedBrightness -
                                               kMinPowerLedBrightness));
    ledcWrite(kPowerLedPwmChannel, brightness);
  }
  return nextLoop;
}

/*
 * A LED matrix that can display frames.
 */
enum MatrixRotation { k000, k090, k180, k270, kNumRotations };
constexpr char MatrixRotationPrefsKey[] = "MatrixRotation";
template <size_t width, size_t height, uint8_t pin>
class LedMatrix {
 private:
  static const size_t kNumLEDs = width * height;
  typedef std::pair<unsigned int, unsigned int> CoordinatePair;
  typedef std::function<CoordinatePair(CoordinatePair)> Transposer;
  CRGB leds[kNumLEDs];
  MatrixRotation rotation;
  Transposer *transposers[MatrixRotation::kNumRotations];

 public:
  LedMatrix() { init(); }

  void init() {
    FastLED.addLeds<NEOPIXEL, pin>(leds, kNumLEDs);
    FastLED.setBrightness(15);
    FastLED.setDither(BINARY_DITHER);
    transposers[MatrixRotation::k000] =
        new Transposer([](CoordinatePair p) -> CoordinatePair { return p; });
    transposers[MatrixRotation::k090] =
        new Transposer([](CoordinatePair p) -> CoordinatePair {
          return CoordinatePair(p.second, (width - 1) - p.first);
        });
    transposers[MatrixRotation::k180] =
        new Transposer([](CoordinatePair p) -> CoordinatePair {
          return CoordinatePair((width - 1) - p.first, (height - 1) - p.second);
        });
    transposers[MatrixRotation::k270] =
        new Transposer([](CoordinatePair p) -> CoordinatePair {
          return CoordinatePair((height - 1) - p.second, p.first);
        });
    setRotation(MatrixRotation::k000);
  }

  void setRotation(MatrixRotation r) { rotation = r; }

  void clear() {
    FastLED.clear();
    FastLED.show();
  }

  void show(const blink::animation::Frame<width, height> &frame) {
    FastLED.clear();
    Transposer *transpose = transposers[rotation];
    frame.CopyToFastLedDisplay([&](size_t y, size_t x, const CRGB &rgb) {
      CoordinatePair transposed = (*transpose)(CoordinatePair(x, y));
      int tgt_index = transposed.second * width +
                      (transposed.second % 2 ? transposed.first
                                             : (width - 1) - transposed.first);
      leds[tgt_index] = rgb;
    });
    FastLED.show();
  }
};

LedMatrix<kLedMatrixNumCols, kLedMatrixNumLines, kLedMatrixDataPin> display;

/*
 * Protocol parser & dispatcher
 *
 * General flow of communications:
 *    --> <CMD>[ <ARGS>...]
 *    <-- (ACK|NAK) CMD[ <ARGS>...| <CAUSE>]
 *
 * Format is strict: commands are three letters, only one space between command,
 * and arguments, everything is case sensitive.
 *
 * Commands:
 *    VER               query firmware version
 *    FRE               query free animation and frame slots
 *    QUE               query animation queue
 *    RST               reset all device data
 *    DBG               dump select engine data for debugging
 *    CLC <RGB>         white color correction point as 3 8-bit hex values
 *    DIM <0..255>      brightness value
 *    DTH ON|OFF        brightness dithering
 *    RGB <RGB STRING>  RGB values for one row (16x3 8-bit hex values) of a
 * frame FRM <MILLIS>      start a frame to display for <MILLIS>ms ANM <MILLIS>
 * start an animation to display for <MILLIS>ms DON               wrap up and
 * enqueue animation NXT               immediately terminate current animation
 * and start the next
 *
 * Example conversation:
 *   --> VER
 *   <-- 1.0
 *   --> ANM 60000
 *   <-- SBY ANM
 *   --> FRM 500
 *   <-- SBY FRM
 *   --> RGB 00FF8800...
 *   <-- ACK RGB 0
 *   --> RGB ...
 *   <-- ACK RGB 1
 *   ...
 *   --> FRM 500
 *   <-- SBY FRM
 *   --> RGB 00FF8800...
 *   <-- ACK RGB
 *   --> RGB ...
 *   ...
 *   --> DNE
 *   <-- ACK ANM
 *   --> QUE
 *   <-- QUE 3725 40000 60000
 *   --> FRE
 *   <-- FRE 2 12
 *   --> RST
 *   <-- ACK RST
 */
#define BUFLEN 100
char inputBuffer[BUFLEN];
char *bufP;
bool lineTooLong;

blink::animation::Animatior<32, 16, kLedMatrixNumCols, kLedMatrixNumLines>
    animator(millis);

namespace bt {
BluetoothSerial serial;
bool pair_request_pending = false;
bool active = false;
bool setup_in_progress = false;

blink::animation::Frame<kLedMatrixNumCols, kLedMatrixNumLines> bt_logo_frame(
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\xFF', '\xFF', '\xFF',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF',
    '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x83', '\xFD', '\xFF', '\xFF', '\xFF',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\xFF', '\xFF', '\xFF',
    '\x00', '\x83', '\xFD', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD',
    '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD',
    '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\xFF', '\xFF', '\xFF',
    '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD',
    '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD', '\xFF', '\xFF', '\xFF',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\xFF', '\xFF', '\xFF',
    '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x83', '\xFD',
    '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD', '\xFF', '\xFF', '\xFF',
    '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF',
    '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\xFF', '\xFF', '\xFF',
    '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\xFF', '\xFF', '\xFF', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD', '\x00', '\x83', '\xFD',
    '\x00', '\x83', '\xFD', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x00', '\x00');

blink::animation::Frame<kLedMatrixNumCols, kLedMatrixNumLines>
    bt_pair_pin_frame;

void _draw_pair_pin_frame(uint32_t pin) {
  // This is a font bitmask, 40x6, each digit is 4x6.
  static const uint64_t kFont[6] = {
      0x6666277f66LLU, 0x9211688199LLU, 0xb216a68269LLU,
      0x9221f1f297LLU, 0x9241219491LLU, 0x67fe2e747eLLU,
  };

  // There are at least 4 digits in the pin and we draw the bottom 4.
  // These are rows, columns and RGB color for each of the 4 digits.
  // Digits are indexed by their weights (rightmost is 0).
  static const int digit_start_row[4] = {9, 9, 1, 1};
  static const int digit_start_col[4] = {10, 2, 10, 2};
  static const uint8_t digit_rgb[12] = {
      0x42, 0x85, 0xF4, 0xDB, 0x44, 0x37, 0xF4, 0xB4, 0x00, 0x0F, 0x9D, 0x58,
  };

  bt_pair_pin_frame.Clear();

  // Cut out digit by digit from the right and draw it.
  for (int digit_idx = 0; digit_idx < 4; digit_idx++) {
    int d = pin % 10;
    pin /= 10;
    for (int digit_row = 0; digit_row < 6; ++digit_row) {
      int row = digit_start_row[digit_idx] + digit_row;
      uint64_t mask = 1LLU << ((9 - d) * 4 + 3);
      for (int digit_col = 0; digit_col < 4; ++digit_col) {
        int col = digit_start_col[digit_idx] + digit_col;
        if (mask & kFont[digit_row]) {
          bt_pair_pin_frame.SetPixel(row, col, digit_rgb[digit_idx * 3],
                                     digit_rgb[digit_idx * 3 + 1],
                                     digit_rgb[digit_idx * 3 + 2]);
        }
        mask >>= 1;
      }
    }
  }
}

void _pair_confirm_request_callback(uint32_t pin) {
  _draw_pair_pin_frame(pin);
  pair_request_pending = true;
}

void _pair_complete_callback(bool success) {
  pair_request_pending = false;
  active = success;
  setup_in_progress = !success;
  if (success) {
    Serial.flush();
  }
}

void Setup() {
  serial.enableSSP();
  serial.onConfirmRequest(_pair_confirm_request_callback);
  serial.onAuthComplete(_pair_complete_callback);
  serial.begin("Blinky");
  setup_in_progress = true;
}

void PairAccept() { serial.confirmReply(true); }
}  // namespace bt

// Stream picking function. If bluetooth is active, it takes precedence.
inline Stream &Comm() {
  if (bt::active) {
    return bt::serial;
  }
  return Serial;
}

void ReportPower(UsbCurrentAvailable pwr) {
  switch (pwr) {
    case UsbCurrentAvailable::kNone:
      Comm().println(F("PWR ???A"));
      break;
    case UsbCurrentAvailable::k3A:
      Comm().println(F("PWR 3.0A"));
      break;
    case UsbCurrentAvailable::k1_5A:
      Comm().println(F("PWR 1.5A"));
      break;
    default:
      Comm().println(F("PWR 0.5A"));
  }
}

void ProcessCommand() {
  static blink::animation::Frame<kLedMatrixNumCols, kLedMatrixNumLines>
      *frame_being_loaded = nullptr;
  int l = bufP - inputBuffer;
  if (lineTooLong) {
    Comm().println(F("NAK LIN"));
    return;
  }
  if (l > 4) {
    if (!strncmp((const char *)F("CLC "), inputBuffer, 4)) {
      CRGB c;
      if (!blink::util::ParseHex(reinterpret_cast<uint8_t *>(&c),
                                 inputBuffer + 4, bufP)) {
        Comm().println(F("NAK CLC ARG"));
        return;
      }
      FastLED.setCorrection(c);
      Comm().print(F("ACK CLC "));
      Comm().print(c.red, HEX);
      Comm().print(c.green, HEX);
      Comm().println(c.blue, HEX);
      return;
    }
    if (!strncmp("DIM ", inputBuffer, 4)) {
      uint32_t b;
      if (!blink::util::ParseUInt32(&b, inputBuffer + 4, bufP) || b > 255) {
        Comm().println(F("NAK DIM ARG"));
        return;
      }
      FastLED.setBrightness(b);
      Comm().print(F("ACK DIM "));
      Comm().print(b);
      return;
    }
    if (!strncmp("DTH ", inputBuffer, 4)) {
      if (l == 6 && !strncmp("ON", inputBuffer + 4, 2)) {
        FastLED.setDither(BINARY_DITHER);
        Comm().println(F("ACK DTH ON"));
        return;
      }
      if (l == 7 && !strncmp("OFF", inputBuffer + 4, 3)) {
        FastLED.setDither(DISABLE_DITHER);
        Comm().println(F("ACK DTH OFF"));
        return;
      }
      Comm().println(F("NAK DTH ARG"));
      return;
    }
    if (!strncmp("RGB ", inputBuffer, 4)) {
      if (frame_being_loaded == nullptr) {
        Comm().println(F("NAK RGB NFM"));
        return;
      }
      if (frame_being_loaded->IsDone()) {
        Comm().println(F("NAK RGB OFL"));
        frame_being_loaded = nullptr;
        return;
      }

      size_t row_being_loaded = frame_being_loaded->RowBeingLoaded();
      if (l != BUFLEN || !frame_being_loaded->LoadPartFromAsciiHexBuffer(
                             inputBuffer + 4, bufP)) {
        Comm().println(F("NAK RGB ARG"));
        return;
      }
      Comm().print(F("ACK RGB "));
      Comm().println(row_being_loaded);
      if (frame_being_loaded->IsDone()) {
        frame_being_loaded = nullptr;
      }
      return;
    }
    if (!strncmp("FRM ", inputBuffer, 4)) {
      uint32_t duration_milis;
      if (!blink::util::ParseUInt32(&duration_milis, inputBuffer + 4, bufP)) {
        Comm().println(F("NAK FRM ARG"));
        return;
      }
      if (!animator.GetFrameToLoad(&frame_being_loaded)) {
        Comm().println(F("NAK FRM UFL"));
        return;
      }
      frame_being_loaded->SetDuration(duration_milis);
      Comm().print(F("ACK FRM "));
      Comm().println(duration_milis);
      return;
    }
    if (!strncmp("ANM ", inputBuffer, 4)) {
      uint32_t duration_milis;
      if (!blink::util::ParseUInt32(&duration_milis, inputBuffer + 4, bufP)) {
        Comm().println(F("NAK ANM ARG"));
        return;
      }
      if (!animator.StartLoadingAnimation(duration_milis)) {
        Comm().println(F("NAK ANM UFL"));
        return;
      }
      Comm().print(F("ACK ANM "));
      Comm().println(duration_milis);
      return;
    }
    if (!strncmp("PWR ", inputBuffer, 4)) {
      if (!strncmp("RST", inputBuffer + 4, 3)) {
        ResetPowerOverride();
      } else if (!strncmp("3.0A", inputBuffer + 4, 4)) {
        SetPowerOverride(UsbCurrentAvailable::k3A);
      } else if (!strncmp("1.5A", inputBuffer + 4, 4)) {
        SetPowerOverride(UsbCurrentAvailable::k1_5A);
      } else if (!strncmp("0.5A", inputBuffer + 4, 4)) {
        SetPowerOverride(UsbCurrentAvailable::kUsbStd);
      } else {
        Comm().println(F("NAK PWR ARG"));
        return;
      }
      Comm().print(F("ACK "));
      ReportPower(PowerOverride);
      return;
    }
    if (l == 7 && !strncmp("ROT ", inputBuffer, 4)) {
      MatrixRotation rotation;
      if (!strncmp("000", inputBuffer + 4, 3)) {
        rotation = MatrixRotation::k000;
      } else if (!strncmp("090", inputBuffer + 4, 3)) {
        rotation = MatrixRotation::k090;
      } else if (!strncmp("180", inputBuffer + 4, 3)) {
        rotation = MatrixRotation::k180;
      } else if (!strncmp("270", inputBuffer + 4, 3)) {
        rotation = MatrixRotation::k270;
      } else {
        Comm().println(F("NAK ROT ARG"));
        return;
      }
      preferences.putUInt(MatrixRotationPrefsKey, (uint8_t)rotation);
      display.setRotation(rotation);
      inputBuffer[7] = '\0';
      Comm().print(F("ACK ROT "));
      Comm().println(inputBuffer + 4);
      return;
    }
  }
  if (l == 3) {
    if (!strncmp("VER", inputBuffer, 3)) {
      Comm().println(F("ACK VER 1.0"));
      return;
    }
    if (!strncmp("PWR", inputBuffer, 3)) {
      Comm().print(F("ACK "));
      ReportPower(currentAvailable);
      return;
    }
    if (!strncmp("QUE", inputBuffer, 3)) {
      Comm().println(F("ACK QUE NOP"));
      return;
    }
    if (!strncmp("FRE", inputBuffer, 3)) {
      Comm().print(F("ACK FRE "));
      Comm().print(animator.GetNumFreeAnimationSlots());
      Comm().print(F(" "));
      Comm().println(animator.GetNumFreeFrameSlots());
      Comm().println();
      return;
    }
    if (!strncmp("DON", inputBuffer, 3)) {
      if (!animator.IsLoadingAnimation()) {
        Comm().println(F("NAK DON NOA"));
        return;
      }
      animator.FinalizeLoadingAnimation();
      Comm().println(F("ACK DON ANM"));
      return;
    }
    if (!strncmp("RST", inputBuffer, 3)) {
      animator.Reset();
      frame_being_loaded = nullptr;
      display.clear();
      Comm().println(F("ACK RST"));
      return;
    }
    if (!strncmp("NXT", inputBuffer, 3)) {
      animator.SkipCurrentAnimation();
      Comm().println(F("ACK NXT"));
      return;
    }
    if (!strncmp("DBG", inputBuffer, 3)) {
      animator.DebugDumpln(Comm());
      return;
    }
  }
  Comm().println(F("NAK CMD"));
}

void SerialInit() {
  Serial.begin(BaudRate);
  Serial.setRxBufferSize(
      SerialRxBufferSize);  // Enough to hold 30ms worth of serial data.
  Serial.println(F("Startup!"));
  bufP = inputBuffer;
  lineTooLong = false;
}

void CommsUpdate() {
  while (Comm().available()) {
    int c = Comm().read();
    if (c == '\n') {
      if (bufP != inputBuffer) {
        ProcessCommand();
      }
      bufP = inputBuffer;
      lineTooLong = false;
    } else if (bufP - inputBuffer < sizeof(inputBuffer)) {
      *bufP++ = c;
    } else {
      lineTooLong = true;
    }
  }
}

// Debouncing touch button controller.
template <int kInputPin>
class TouchButtonControler {
 public:
  TouchButtonControler()
      : filtered_value_(static_cast<float>(touchRead(kInputPin))) {}

  void Update() {
    filtered_value_ *= kFilterStrength;
    filtered_value_ += (1.0f - kFilterStrength) * touchRead(kInputPin);
  }

  bool Pressed() const { return filtered_value_ < kPressThreshold; }

 private:
  static constexpr float kFilterStrength = 0.7f;
  static constexpr float kPressThreshold = 20.0f;
  float filtered_value_;
};

/*
 * And the usual Arduino song and dance.
 */
void setup() {
  ledcAttachPin(kPowerLedPin, kPowerLedPwmChannel);
  ledcSetup(kPowerLedPwmChannel, kPowerLedPwmFrequency, kPowerLedPwmResolution);
  pinMode(kOnboardLed1Pin, OUTPUT);
  pinMode(kLedMatrixDataPin, OUTPUT);
  pinMode(kLedMatrixPowerPin0, OUTPUT);
  pinMode(kLedMatrixPowerPin1, OUTPUT);
  pinMode(kTouch0Pin, INPUT);
  pinMode(kTouch1Pin, INPUT);
  pinMode(kTouch2Pin, INPUT);
  preferences.begin("Blinkenlights", false);
  GetPowerOverride();
  display.clear();
  display.setRotation((MatrixRotation)preferences.getUInt(
      MatrixRotationPrefsKey, (uint8_t)MatrixRotation::k000));
  SerialInit();
}

void loop() {
  uint32_t loop_epoch = millis();
  uint32_t next_loop = PowerUpdate();
  static uint32_t loop_overflow_millis = 0;

  // Handle button presses.
  static TouchButtonControler<kTouch0Pin> button_0;
  static TouchButtonControler<kTouch1Pin> button_1;
  static TouchButtonControler<kTouch2Pin> button_2;
  static uint32_t btn_not_all_pressed_time = millis();

  button_0.Update();
  button_1.Update();
  button_2.Update();

  int btn_num_pressed = 0;
  if (button_0.Pressed()) btn_num_pressed++;
  if (button_1.Pressed()) btn_num_pressed++;
  if (button_2.Pressed()) btn_num_pressed++;

  if (btn_num_pressed < 3) {
    btn_not_all_pressed_time = loop_epoch;
  } else if (loop_epoch - btn_not_all_pressed_time > 3000 &&
             !bt::setup_in_progress && !bt::active) {
    bt::Setup();
  }

  if (bt::setup_in_progress) {
    if (bt::pair_request_pending) {
      display.show(bt::bt_pair_pin_frame);

      if (btn_num_pressed == 1) {
        bt::PairAccept();
      }
    } else {
      display.show(bt::bt_logo_frame);
    }
  } else {
    display.show(animator.GetCurrentFrame());
    CommsUpdate();
  }

  // USB-C spec says the host can change the advertised current limit at any
  // time, and we have tSinkAdj(max) (60ms) to comply. When we detect a CC value
  // change, we also have to wait tRpValueChange(min) (10ms) to make sure this
  // is not a PD message (which cause, for our purposes, noise on the CC line).
  // So we implement that by having the loop run at 30ms, and if we see a CC
  // change, we sample again in 15ms to make sure the value is stable,
  // satisfying both timing requirements. We also light up LED#1 for 5 seconds
  // if the main loop overruns its time allotment. It's meant only for
  // debugging, people will probably not be normally watching this, so it
  // doesn't matter that the logic implies that the LED might *not* light up if
  // the overflow is detected exactly at millis() == 0. We might want to
  // consider logging such events as ASY messages in the future.
  uint32_t elapsed = millis() - loop_epoch;
  if (elapsed > next_loop) {
    loop_overflow_millis = millis();
  } else {
    delay(next_loop - elapsed);
  }
  if (loop_overflow_millis) {
    if (millis() - loop_overflow_millis < 5000) {
      digitalWrite(kOnboardLed1Pin, HIGH);
    } else {
      digitalWrite(kOnboardLed1Pin, LOW);
      loop_overflow_millis = 0;
    }
  }
}
