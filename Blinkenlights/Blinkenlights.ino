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

enum MatrixRotation { k000, k090, k180, k270, kNumRotations };

// LED Matrix dimensions
constexpr int kLedMatrixNumCols = 16;
constexpr int kLedMatrixNumLines = 16;
constexpr int kLedMatrixNumLeds = kLedMatrixNumCols * kLedMatrixNumLines;

typedef struct {
  const char *command;
  void (*implementation)(const char *[], const int);
} DispatchEntry;


/*
 * Called on violation of invariants. May be used for desperate
 * recovery attempts or error status signalling.
 * State is undefined at beginning of the call.
 *
 * Parameters:
 *    err:  informational error code
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

  FastLED.delay(50);

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
        FastLED.setMaxPowerInVoltsAndMilliamps(5, 3000);
        EnableLEDPower();
        break;
      case UsbCurrentAvailable::k1_5A:
        FastLED.setMaxPowerInVoltsAndMilliamps(5, 1500);
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
    FastLED.setDither(DISABLE_DITHER);
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

blink::animation::Animator<32, 16, kLedMatrixNumCols, kLedMatrixNumLines>
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
 *                      frame
 *    FRM <MILLIS>      start a frame to display for <MILLIS>ms
 *    ANM <MILLIS>      start an animation to display for <MILLIS> ms
 *    DON               wrap up and enqueue animation
 *    NXT               immediately terminate current animation
 *                      and start the next
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

#define COMMAND_MAXLEN 100
#define COMMAND_MAXWORDS 4


void ReportError(const char *cmd, const char *err) {
  Comm().print(F("NAK "));
  Comm().print(cmd);
  Comm().print(F(" "));
  Comm().println(err);
}


void ArgumentError(const char *cmd) {
  ReportError(cmd, "ARG");
}


/*
 * Query the protocol version.
 */
void VersionCommand(const char *wordv[], const int wordc) {
  if (wordc == 1) {
    Comm().print(F("ACK "));
    Comm().print(wordv[0]);
    Comm().println(F(" 1.0"));
  } else {
    ArgumentError(wordv[0]);
  }
}


/*
 * Power management.
 */
void ReportPower(const char *cmd, UsbCurrentAvailable pwr) {
  Comm().print(F("ACK "));
  Comm().print(cmd);
  switch (pwr) {
    case UsbCurrentAvailable::kNone:
      Comm().println(F(" ???A"));
      break;
    case UsbCurrentAvailable::k3A:
      Comm().println(F(" 3.0A"));
      break;
    case UsbCurrentAvailable::k1_5A:
      Comm().println(F(" 1.5A"));
      break;
    default:
      Comm().println(F(" 0.5A"));
  }
}


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


void PowerCommand(const char *wordv[], const int wordc) {
  switch (wordc) {
  case 2:
    if (!strcmp("RST", wordv[1])) {
      ResetPowerOverride();
      Serial.print(F("ACK "));
      Serial.print(wordv[0]);
      Serial.println(F(" RST"));
      return;
    } else if (!strcmp("3.0A", wordv[1])) {
      SetPowerOverride(UsbCurrentAvailable::k3A);
    } else if (!strcmp("1.5A", wordv[1])) {
      SetPowerOverride(UsbCurrentAvailable::k1_5A);
    } else if (!strcmp("0.5A", wordv[1])) {
      SetPowerOverride(UsbCurrentAvailable::kUsbStd);
    } else {
      ArgumentError(wordv[0]);
      return;
    }
    // FALL THROUGH!
  case 1:
    ReportPower(wordv[0], PowerOverride == UsbCurrentAvailable::kNone ? currentAvailable : PowerOverride);
    break;
  default:
    ArgumentError(wordv[0]);
  }
}


//TODO: I seem to have forgottent to implement this?
void QueueCommand(const char *wordv[], const int wordc) {
  if (wordc == 1) {
    Comm().print(F("NAK "));
    Comm().print(wordv[0]);
    Comm().println(F(" NOP"));
  } else {
    ArgumentError(wordv[0]);
  }
}


/*
 * Query free animation storage.
 */
void FreeCommand(const char *wordv[], const int wordc) {
  if (wordc == 1) {
    Comm().print(F("ACK FRE "));
    Comm().print(animator.GetNumFreeAnimationSlots());
    Comm().print(F(" "));
    Comm().println(animator.GetNumFreeFrameSlots());
  } else {
    ArgumentError(wordv[0]);
  }
}


/*
 * Skip to next animation in queue.
 */
void NextCommand(const char *wordv[], const int wordc) {
  if (wordc == 1) {
    animator.SkipCurrentAnimation();
    Comm().print(F("ACK "));
    Comm().println(wordv[0]);
  } else {
    ArgumentError(wordv[0]);
  }
}


/*
 * Debug dump (not really part of the protocol).
 */
void DebugCommand(const char *wordv[], const int wordc) {
  if (wordc == 1) {
      animator.DebugDumpln(Comm());
  } else {
    ArgumentError(wordv[0]);
  }
}


/*
 * Color Correction preference management.
 */
constexpr char ColorCorrectionPrefsKey[] = "ColorCorrection";
void SetColorCorrectionPref(CRGB cc) {
  preferences.putUInt(ColorCorrectionPrefsKey, (cc.red<<16)|(cc.green<<8)|cc.blue);
}


CRGB GetColorCorrectionPref() {
  return CRGB(preferences.getUInt(ColorCorrectionPrefsKey, LEDColorCorrection::Typical8mmPixel));
}


void ResetColorCorrectionPref() {
  preferences.remove(ColorCorrectionPrefsKey);
}


void ColorCorrectCommand(const char *wordv[], const int wordc) {
  CRGB c;

  switch (wordc) {
  case 2:
      if (!strcmp("RST", wordv[1])) {
        ResetColorCorrectionPref();
        FastLED.setCorrection(GetColorCorrectionPref());
      } else {
        if (!blink::util::ParseHex(c.raw, wordv[1])) {
	  ArgumentError(wordv[0]);
          return;
        }
        SetColorCorrectionPref(c);
      }
  case 1:
    c = GetColorCorrectionPref();
    FastLED.setCorrection(c);
    Comm().print(F("ACK "));
    Comm().print(wordv[0]);
    Comm().print(F(" "));
    Comm().print(c.red, HEX);
    Comm().print(c.green, HEX);
    Comm().println(c.blue, HEX);
    break;
  default:
    ArgumentError(wordv[0]);
  }
}


/*
 * Brightness preference management.
 */
// TODO: this should be stored in preferences.
void BrightnessCommand(const char *wordv[], const int wordc) {
  uint32_t b;

  switch (wordc) {
  case 2:
    if (!blink::util::ParseUInt32(&b, wordv[1]) || b > 255) {
      ArgumentError(wordv[0]);
      return;
    }
    FastLED.setBrightness(b);
  case 1:
    b = FastLED.getBrightness();
    Comm().print(F("ACK "));
    Comm().print(wordv[0]);
    Comm().print(F(" "));
    Comm().println(b);
    break;
  default:
    ArgumentError(wordv[0]);
  }
}


/*
 * Color dithering preference management.
 */
// TODO: this should be stored in preferences.
void DitherCommand(const char *wordv[], const int wordc) {
  switch (wordc) {
  case 2:
      if (!strcmp("ON", wordv[1])) {
        FastLED.setDither(BINARY_DITHER);
      } else if (!strcmp("OFF", wordv[1])) {
        FastLED.setDither(DISABLE_DITHER);
      } else {
	ArgumentError(wordv[0]);
	return;
      }
      Comm().print(F("ACK "));
      Comm().print(wordv[0]);
      Comm().print(F(" "));
      Comm().println(wordv[1]);
      break;
  case 1:
  default:
    ArgumentError(wordv[0]);
  }
}


/*
 * Rotation preference management.
 */
constexpr char MatrixRotationPrefsKey[] = "MatrixRotation";


MatrixRotation GetRotationPref() {
  return (MatrixRotation)preferences.getUInt(MatrixRotationPrefsKey, (uint8_t)MatrixRotation::k000);
}


void SetRotationPref(MatrixRotation rot) {
  preferences.putUInt(MatrixRotationPrefsKey, (uint8_t)rot);
}


void ReportRotation(const char *cmd, MatrixRotation rot) {
  Comm().print(F("ACK "));
  Comm().print(cmd);
  switch (rot) {
    case MatrixRotation::k000:
      Comm().println(F(" 000"));
      break;
    case MatrixRotation::k090:
      Comm().println(F(" 090"));
      break;
    case MatrixRotation::k180:
      Comm().println(F(" 180"));
      break;
    case MatrixRotation::k270:
      Comm().println(F(" 270"));
      break;
  }
}


void RotateCommand(const char *wordv[], const int wordc) {
  MatrixRotation rotation;
  switch (wordc) {
  case 2:
    if (!strcmp("000", wordv[1])) {
      rotation = MatrixRotation::k000;
    } else if (!strcmp("090", wordv[1])) {
      rotation = MatrixRotation::k090;
    } else if (!strcmp("180", wordv[1])) {
      rotation = MatrixRotation::k180;
    } else if (!strcmp("270", wordv[1])) {
      rotation = MatrixRotation::k270;
    } else {
      ArgumentError(wordv[0]);
      return;
    }
    SetRotationPref(rotation);
  case 1:
    rotation = GetRotationPref();
    display.setRotation(rotation);
    ReportRotation(wordv[0], rotation);
    break;
  default:
    ArgumentError(wordv[0]);
  }
}


blink::animation::Frame<kLedMatrixNumCols, kLedMatrixNumLines> *FrameBeingLoaded = nullptr;

/*
 * Get an RGB line
 */
void RgbCommand(const char *wordv[], const int wordc) {
  if (FrameBeingLoaded == nullptr) {
    ReportError(wordv[0], "NFM");
    return;
  }
  if (FrameBeingLoaded->IsDone()) {
    ReportError(wordv[0], "OFL");
    FrameBeingLoaded = nullptr;
    return;
  }
  size_t row_being_loaded = FrameBeingLoaded->RowBeingLoaded();
  if (!FrameBeingLoaded->LoadPartFromAsciiHexBuffer(wordv[1])) {
    ArgumentError(wordv[0]);
    return;
  }
  Comm().print(F("ACK "));
  Comm().print(wordv[0]);
  Comm().print(F(" "));
  Comm().println(row_being_loaded);
  if (FrameBeingLoaded->IsDone()) {
    FrameBeingLoaded = nullptr;
  }
}


/*
 * Start loading a frame
 */
void FrameCommand(const char *wordv[], const int wordc) {
  uint32_t duration_milis;

  if (!blink::util::ParseUInt32(&duration_milis, wordv[1])) {
    ArgumentError(wordv[0]);
    return;
  }
  if (!animator.GetFrameToLoad(&FrameBeingLoaded)) {
    ReportError(wordv[0], "UFL");
    return;
  }
  FrameBeingLoaded->SetDuration(duration_milis);
  Comm().print(F("ACK "));
  Comm().print(wordv[0]);
  Comm().print(" ");
  Comm().println(duration_milis);
  return;
}


/*
 * Start loading an animation
 */
void AnimationCommand(const char *wordv[], const int wordc) {
  uint32_t duration_milis;

  if (!blink::util::ParseUInt32(&duration_milis, wordv[1])) {
    ArgumentError(wordv[0]);
    return;
  }
  if (!animator.StartLoadingAnimation(duration_milis)) {
    ReportError(wordv[0], "UFL");
    return;
  }
  Comm().print(F("ACK "));
  Comm().print(wordv[0]);
  Comm().print(" ");
  Comm().println(duration_milis);
  return;
}


/*
 * Reset animation machinery and display.
 */
void ResetCommand(const char *wordv[], const int wordc) {
  if (wordc == 1) {
    animator.Reset();
    FrameBeingLoaded = nullptr;
    display.clear();
    Comm().print(F("ACK "));
    Comm().println(wordv[0]);
  } else {
    ArgumentError(wordv[0]);
  }
}


/*
 * Close animation upload.
 */
void DoneCommand(const char *wordv[], const int wordc) {
  if (wordc == 1) {
    if (!animator.IsLoadingAnimation()) {
      Comm().println(F("NAK DON NOA"));
      return;
    }
    animator.FinalizeLoadingAnimation();
    Comm().print(F("ACK "));
    Comm().print(wordv[0]);
    Comm().println(F(" ANM"));
  } else {
    ArgumentError(wordv[0]);
  }
}


/*
 * Command dispatching.
 */
DispatchEntry DispatchTable[] = {
    {"ANM", AnimationCommand},
    {"CLC", ColorCorrectCommand},
    {"DBG", DebugCommand},
    {"DIM", BrightnessCommand},
    {"DON", DoneCommand},
    {"DTH", DitherCommand},
    {"FRE", FreeCommand},
    {"FRM", FrameCommand},
    {"NXT", NextCommand},
    {"PWR", PowerCommand},
    {"QUE", QueueCommand},
    {"RGB", RgbCommand},
    {"ROT", RotateCommand},
    {"RST", ResetCommand},
    {"VER", VersionCommand}  
};

constexpr int NumCommands = sizeof(DispatchTable)/sizeof(DispatchTable[0]);

const DispatchEntry *getDispatch(const char *cmd, const DispatchEntry commands[], const int numCommands) {
  int lo = 0;
  int hi = numCommands;

  if (strlen(cmd) != 3) return NULL;
  while (hi > lo) {
    int mid = (hi+lo)/2;
    int cmp = strcmp(cmd, commands[mid].command);
    if (cmp < 0) {
      hi = mid;
    } else if (cmp > 0) {
      lo = mid+1;
    } else {
      return &commands[mid];
    }
  }
  return NULL;
}

void ProcessCommand(const char *wordv[], const int wordc) {
  const DispatchEntry *dispatch = getDispatch(wordv[0], DispatchTable, NumCommands);

  if (dispatch == NULL) {
    Comm().println(F("NAK CMD"));
    return;
  }
  dispatch->implementation(wordv, wordc);
}
  
  
char inputBuffer[COMMAND_MAXLEN+1]; // Make space for string terminator
char *endOfBuffer;
bool lineTooLong;

void SerialInit() {
  Serial.begin(BaudRate);
  Serial.setRxBufferSize(
      SerialRxBufferSize);  // Enough to hold 30ms worth of serial data.
  Serial.println(F("Startup!"));
  endOfBuffer = inputBuffer;
  lineTooLong = false;
}

void CommsUpdate() {
  while (Comm().available()) {
    int c = Comm().read();
    if (c == '\n') {
      if (endOfBuffer != inputBuffer) {
        char *wordv[COMMAND_MAXWORDS];
        int wordc = 0;
        char *p = inputBuffer;

        *endOfBuffer = '\0';
        if (lineTooLong)
          Comm().println(F("NAK LTL"));

        for (p = inputBuffer; *p && wordc <= COMMAND_MAXWORDS;) {
          while (*p && isspace(*p)) p++;
          if (*p) {
            wordv[wordc++] = p;
            while (*p && !isspace(*p)) p++;
            if (*p) *(p++) = '\0';
          }
        }

        if (*p) {
          Comm().println(F("NAK LIN"));
        } else {
          ProcessCommand((const char **)wordv, wordc);
        }
      }
      endOfBuffer = inputBuffer;
      lineTooLong = false;
    } else if (endOfBuffer - inputBuffer < COMMAND_MAXLEN) {
      *endOfBuffer++ = c;
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
  FastLED.setCorrection(GetColorCorrectionPref());
  display.clear();
  display.setRotation(GetRotationPref());
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
    FastLED.delay(next_loop - elapsed);
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
