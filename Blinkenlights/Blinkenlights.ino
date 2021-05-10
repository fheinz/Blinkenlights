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

#include "FastLED.h"       // Fastled library to control the LEDs


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
constexpr float kMatrixMaxCurrent = kLedMatrixNumLeds * 0.06f; // WS2812B: 60mA/LED
constexpr float kMaxIdleCurrent = 0.5f; // Matrix + ESP32 idle


/*
 * Animation frames
 * 
 * Each frame contains an array of RGB images representing the image,
 * flattened as a sequence of rows, and a duration for which the image
 * should be shown.
 */
typedef uint8_t FrameIndex;
typedef uint32_t Duration;
typedef struct {
  uint8_t pixels[kLedMatrixNumLeds*3];
  Duration duration;
  FrameIndex next;
} AnimationFrame;
constexpr FrameIndex kFramesSentinel = 255;

/*
 * Animations
 * 
 * Each animation contains the list of frames that make it up,
 * and a total duration for the animation. If the animation's
 * duration is longer than the sum of the frames' durations,
 * it will cycle through them.
 */
typedef uint8_t AnimationIndex;
typedef uint8_t AnimationPosition;
typedef struct {
  Duration duration;
  FrameIndex frames;
  AnimationIndex next;
} Animation;
constexpr AnimationIndex kAnimationsSentinel = 255;

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
 * String --> integer parsing utility functions
 */

inline bool isdigit(char c) { return c >= '0' && c <= '9'; }
inline bool ishexdigit(char c) { return c >= 'A' && c <= 'F'; }

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
bool ParseUInt32(uint32_t *i, const char *from, const char *to) {
  uint32_t acc = 0;
  while (from < to){
    char c = *from++;
    if (!isdigit(c))
      return false;
    acc = acc * 10 + (c - '0');
  }
  *i = acc;
  return true;
}

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
bool ParseHex(uint8_t *buf, const char *from, const char *to) {
  bool half = false;
  while (from < to) {
    char c = *from++;
    uint8_t d;
    if (isdigit(c)) {
      d = c -'0';
    } else if (ishexdigit(c)) {
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


/* 
 * Read voltage at an input pin.
 * 
 * By default we have 11db (1/3.6) attenuation with 1.1V reference,
 * so full scale voltage range is 1.1*3.6 = 3.96. In reality it's
 * clamped to Vdd (3.3V) so we will never get a reading above that,
 * but we need to the calculations using 3.96.
 */
float AnalogReadV(int pin) {
  return analogRead(pin) * 3.96f / 4096;
}
 
/*
 * Detect USB-C current advertisement according to Universal 
 * Serial Bus Type-C Cable and Connector Specification
 * Depending on cable orientation, either CC1 or CC2 will be >0V,
 * and that tells us how much current we can draw.
 * Voltage thresholds from Table 4-36:
 * >0.2V = connected, >0.66V = 1.5A, >1.23V = 3A. 
 */
UsbCurrentAvailable DetermineMaxCurrent() {
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

void ReportPower() {
  switch (currentAvailable) {
    case UsbCurrentAvailable::kNone:
      Serial.println(F("PWR ???A"));
      break;
    case UsbCurrentAvailable::k3A:
      Serial.println(F("PWR 3.0A"));
      break;
    case UsbCurrentAvailable::k1_5A:
      Serial.println(F("PWR 1.5A"));
      break;
    default:
      Serial.println(F("PWR 0.5A"));
  } 
}

uint32_t PowerUpdate() {
  UsbCurrentAvailable current_advertisement = DetermineMaxCurrent();
  if (currentAvailable == current_advertisement) 
    goto done;
  if (currentAvailableDetected == current_advertisement) {
    // We detected this change 15ms ago, so it can't be a PD message
    // because those take 10ms max.
    switch (current_advertisement) {
      case UsbCurrentAvailable::k3A:
        digitalWrite(kOnboardLed0Pin, HIGH);
        digitalWrite(kOnboardLed1Pin, HIGH);
        FastLED.setBrightness(255 * (3.0f - kMaxIdleCurrent) / kMatrixMaxCurrent);
        EnableLEDPower();
        break;
      case UsbCurrentAvailable::k1_5A:
        digitalWrite(kOnboardLed0Pin, LOW);
        digitalWrite(kOnboardLed1Pin, HIGH);
        FastLED.setBrightness(255 * (1.5f - kMaxIdleCurrent) / kMatrixMaxCurrent);
        EnableLEDPower();
        break;
      default:
        digitalWrite(kOnboardLed0Pin, LOW);
        digitalWrite(kOnboardLed1Pin, LOW);
        DisableLEDPower();
    }
    currentAvailable = current_advertisement;
    ReportPower();
  done:
    currentAvailableDetected = UsbCurrentAvailable::kNone;
    return 30;
  }
  // We just detected this change... check back in 15ms to see if it's still there.
  currentAvailableDetected = current_advertisement;
  return 15;
}

/*
 * A LED matrix that can display frames.
 */
template <size_t matrixSize, uint8_t pin> class LedMatrix {
  private:
    CRGB leds[matrixSize];

  public:
    LedMatrix() {
      init();
    }

    void init() {
      FastLED.addLeds<NEOPIXEL, pin>(leds, matrixSize);
      FastLED.setBrightness(15);
      FastLED.setDither(BINARY_DITHER);
    }

    void clear() {
      FastLED.clear();
      FastLED.show();
    }
  
    void show(const AnimationFrame &f) {
      const uint8_t *pixels = f.pixels;
      FastLED.clear();
      for (int line = 0; line < kLedMatrixNumLines; line++) {
        for (int col = 0; col < kLedMatrixNumCols; col++) {
          int tgt_index = line * kLedMatrixNumCols + (line % 2 ? col : (kLedMatrixNumCols - 1) - col);
          leds[tgt_index] = CRGB(pixels[0], pixels[1], pixels[2]);  
          pixels += 3;
        }
      }
      FastLED.show();
    }
};


LedMatrix<kLedMatrixNumLeds, kLedMatrixDataPin> display;

/*
 * The frames pool.
 */
constexpr int kMaxNumFrames = 32;
AnimationFrame frames[kMaxNumFrames];
FrameIndex freeFrames;
uint8_t numFreeFrames;

void FramesReset() {
    for (int i = 0; i < kMaxNumFrames-1; i++) {
    frames[i].next = i+1;
  }
  frames[kMaxNumFrames-1].next = kFramesSentinel;
  freeFrames = (FrameIndex)0;
  numFreeFrames = kMaxNumFrames;
}

FrameIndex FramesGetFrame() {
  FrameIndex f = freeFrames;
  if (f != kFramesSentinel) {
    freeFrames = frames[f].next;
    frames[f].next = kFramesSentinel;
    numFreeFrames--;
  }
  return f;
}


void FramesFreeFrames(FrameIndex f) {
  while (f != kFramesSentinel) {
    if (f >= kMaxNumFrames) {
      CantHappen(ErrorCode::kInvalidFrameIndex);
    }
    FrameIndex n = frames[f].next;
    frames[f].next = freeFrames;
    freeFrames = f;
    f = n;
    numFreeFrames++;
  }
}


int FramesCountFrames(FrameIndex f) {
  int count = 0;
  
  while (f != kFramesSentinel) {
    count++;
    f = frames[f].next;
  }
  return count;
}


/*
 * The animations pool.
 */
constexpr int kMaxNumAnimations = 16;
Animation animations[kMaxNumAnimations];
AnimationIndex freeAnimations;
uint8_t numFreeAnimations;
AnimationIndex liveAnimations;
uint8_t numLiveAnimations;

void AnimationsReset() {
  for (int i = 0; i < kMaxNumAnimations-1; i++) {
    animations[i].next = i+1;
  }
  animations[kMaxNumAnimations-1].next = kAnimationsSentinel;
  freeAnimations = (AnimationIndex)0;
  liveAnimations = kAnimationsSentinel;
  numFreeAnimations = kMaxNumAnimations;
  numLiveAnimations = 0;
}

AnimationIndex AnimationsGetAnimation() {
  AnimationIndex a = freeAnimations;
  if (a != kAnimationsSentinel) {
    freeAnimations = animations[a].next;
    numFreeAnimations--;
    animations[a].duration = 0;
    animations[a].frames = kFramesSentinel;
    animations[a].next = kAnimationsSentinel;
  }
  return a;
}

void AnimationsFreeAnimation(AnimationIndex a) {
  while (a != kAnimationsSentinel) {
    if (a >= kMaxNumAnimations) {
      CantHappen(ErrorCode::kEnqueueInvalidAnimation);
    }
    FramesFreeFrames(animations[a].frames);
    animations[a].frames = kFramesSentinel;
    AnimationIndex n = animations[a].next;
    animations[a].next = freeAnimations;
    freeAnimations = a;
    a = n;
    numFreeAnimations++;
  }
}

/*
 * Building/enqueueing/destroying animations.
 */
void AnimationsAddFrame(AnimationIndex a, FrameIndex f) {
  if (a == kAnimationsSentinel) {
    CantHappen(ErrorCode::kAddFrameToInvalidAnimation);
    return;
  }
  if (f == kFramesSentinel) {
    CantHappen(ErrorCode::kAddInvalidFrameToAnimation);
    return;
  }
  FrameIndex *cur = &animations[a].frames;
  while (*cur != kFramesSentinel) {
    cur = &(frames[*cur].next);
  }
  *cur = f;
  frames[f].next = kFramesSentinel;
}

void AnimationsEnqueueAnimation(AnimationIndex a) {
  if (a == kAnimationsSentinel) {
    CantHappen(ErrorCode::kAddFrameToInvalidAnimation);
    return;
  }
  AnimationIndex *cur = &liveAnimations;
  while (*cur != kAnimationsSentinel) {
    cur = &(animations[*cur].next);
  }
  animations[a].next = kAnimationsSentinel;
  *cur = a;
  numLiveAnimations++;
}

void AnimationsDequeueAnimation() {
  AnimationIndex cur = liveAnimations;
  if (cur == kAnimationsSentinel) 
    return;
  liveAnimations = animations[cur].next;
  animations[cur].next = kAnimationsSentinel;
  numLiveAnimations--;
  if (liveAnimations == kAnimationsSentinel) {
    display.clear();
  }
  AnimationsFreeAnimation(cur);
}

/*
 * The animation engine.
 */
FrameIndex nextFrame;             // Next frame to display
                                  // If kFramesSentinel ==> no active animation
uint32_t animationEpoch;          // Time at which the animation started
uint32_t animationClock;          // Time elapsed since animationEpoch
uint32_t frameTransitionTime;     // Time after Epoch at which the a new frame is due
uint32_t animationTransitionTime; // Time after Epoch at which the current animation should end
bool skipToNextAnimation;         // true ==> discard the current animation now

void AnimationUpdate() {
  if (nextFrame != kFramesSentinel && (animationClock >= animationTransitionTime || skipToNextAnimation)) {
    AnimationsDequeueAnimation();
    nextFrame = kFramesSentinel;
  }
  skipToNextAnimation = false;
  if (nextFrame == kFramesSentinel) {
    if (liveAnimations != kAnimationsSentinel) {
      nextFrame = animations[liveAnimations].frames;
      animationEpoch = animationClock + animationEpoch;
      frameTransitionTime = animationClock = 0;
      animationTransitionTime = animations[liveAnimations].duration; 
    }
  }
  if (nextFrame != kFramesSentinel && animationClock >= frameTransitionTime) {
    display.show(frames[nextFrame]);
    frameTransitionTime = animationClock + frames[nextFrame].duration;
    nextFrame = frames[nextFrame].next;
    if (nextFrame == kFramesSentinel) {
      nextFrame = animations[liveAnimations].frames;
    }
  }
}

void AnimationInit() {
  nextFrame = kFramesSentinel;
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
 *    RGB <RGB STRING>  RGB values for one row (16x3 8-bit hex values) of a frame
 *    FRM <MILLIS>      start a frame to display for <MILLIS>ms
 *    ANM <MILLIS>      start an animation to display for <MILLIS>ms
 *    DON               wrap up and enqueue animation
 *    NXT               immediately terminate current animation and start the next
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

AnimationIndex animationInProgress;
FrameIndex frameInProgress;
int frameInProgressLine;

void CloseFrameConstruction() {
  if (frameInProgress != kFramesSentinel) {
    AnimationsAddFrame(animationInProgress, frameInProgress);
    frameInProgress = kFramesSentinel;
    frameInProgressLine = 0;
  }  
}

void CloseAnimationConstruction() {
  if (animationInProgress != kAnimationsSentinel) {
    CloseFrameConstruction();
    AnimationsEnqueueAnimation(animationInProgress);
    animationInProgress = kAnimationsSentinel;
  }
}

void ProcessCommand() {
  int l = bufP-inputBuffer;
  if (lineTooLong) {
    Serial.println(F("NAK LIN"));
    return;
  }
  if (l > 4) {
    if (!strncmp((const char *)F("CLC "), inputBuffer, 4)) {
      CRGB c;
      if (!ParseHex((uint8_t *)&c, inputBuffer+4, bufP)) {
        Serial.println(F("NAK CLC ARG"));
        return;
      }
      FastLED.setCorrection(c);
      Serial.print(F("ACK CLC "));
      Serial.print(c.red, HEX);
      Serial.print(c.green, HEX);
      Serial.println(c.blue, HEX);
      return;
    }
    if (!strncmp("DIM ", inputBuffer, 4)) {
      uint32_t b;
      if (!ParseUInt32(&b, inputBuffer+4, bufP) || b > 255) {
        Serial.println(F("NAK DIM ARG"));
        return;
      }
      FastLED.setBrightness(b);
      Serial.print(F("ACK DIM "));
      Serial.print(b);
      return;
    }
    if (!strncmp("DTH ", inputBuffer, 4)) {
      if (l == 6 && !strncmp("ON", inputBuffer+4, 2)) {
        FastLED.setDither(BINARY_DITHER);       
        Serial.println(F("ACK DTH ON"));
        return;
      }
      if (l == 7 && !strncmp("OFF", inputBuffer+4, 3)) {
        FastLED.setDither(DISABLE_DITHER);
        Serial.println(F("ACK DTH OFF"));
        return;
      }
      Serial.println(F("NAK DTH ARG"));
      return;
    }
    if (!strncmp("RGB ", inputBuffer, 4)) {
      if (frameInProgress == kFramesSentinel) {
        Serial.println(F("NAK RGB NFM"));
        return;
      }
      if (frameInProgressLine >= kLedMatrixNumLines) {
        Serial.println(F("NAK RGB OFL"));
        return;        
      }
      if (l != 100 || !ParseHex(&(frames[frameInProgress].pixels[frameInProgressLine*3*kLedMatrixNumCols]), inputBuffer+4, bufP)) {
        Serial.println(F("NAK RGB ARG"));
        return;
      }
      Serial.print(F("ACK RGB "));
      Serial.println(frameInProgressLine);
      frameInProgressLine++;
      return;
    }
    if (!strncmp("FRM ", inputBuffer, 4)) {
      Duration d;
      if (!ParseUInt32(&d, inputBuffer+4, bufP)) {
        Serial.println(F("NAK FRM ARG"));
        return;
      }
      CloseFrameConstruction();
      frameInProgress = FramesGetFrame();    
      if (frameInProgress == kFramesSentinel) {
        Serial.println(F("NAK FRM UFL"));
        return;
      }
      frames[frameInProgress].duration = d;
      Serial.print(F("ACK FRM "));
      Serial.println(d);
      return;   
    }
    if (!strncmp("ANM ", inputBuffer, 4)) {
      Duration d;
      if (!ParseUInt32(&d, inputBuffer+4, bufP)) {
        Serial.println(F("NAK ANM ARG"));
        return;
      }
      CloseAnimationConstruction();
      animationInProgress = AnimationsGetAnimation();
      if (animationInProgress == kAnimationsSentinel) {
        Serial.println(F("NAK ANM UFL"));
        return;
      }
      animations[animationInProgress].duration = d;
      Serial.print(F("ACK ANM "));
      Serial.println(d);
      return;   
    }
  }
  if (l == 3) {
    if (!strncmp("VER", inputBuffer, 3)) {
      Serial.println(F("ACK VER 1.0"));
      return;
    }
    if (!strncmp("PWR", inputBuffer, 3)) {
      Serial.print(F("ACK "));
      ReportPower();
      return;
    }
    if (!strncmp("QUE", inputBuffer, 3)) {
      AnimationIndex a = liveAnimations;
      Serial.print(F("ACK QUE"));
      if (a != kAnimationsSentinel) {
        Serial.print(F(" ("));
        Serial.print(animations[a].duration-animationClock);
        Serial.print(F(", "));
        Serial.print(FramesCountFrames(animations[a].frames));
        Serial.print(F(")"));
        a = animations[a].next;
      }
      while (a != kAnimationsSentinel) {
        Serial.print(F(" ("));
        Serial.print(animations[a].duration);
        Serial.print(F(", "));
        Serial.print(FramesCountFrames(animations[a].frames));
        Serial.print(F(")"));
        a = animations[a].next;
      }
      Serial.println();
      return;
    }
    if (!strncmp("FRE", inputBuffer, 3)) {
      Serial.print(F("ACK FRE "));
      Serial.print(numFreeAnimations);
      Serial.print(F(" "));
      Serial.println(numFreeFrames);
      return;
    }
    if (!strncmp("DON", inputBuffer, 3)) {
      if (animationInProgress == kAnimationsSentinel) {
        Serial.println(F("NAK DON NOA"));
        return;
      }
      CloseAnimationConstruction();
      Serial.println(F("ACK DON ANM"));
      return;
    }
    if (!strncmp("RST", inputBuffer, 3)) {
      CloseAnimationConstruction();
      while (liveAnimations != kAnimationsSentinel) {
        AnimationsDequeueAnimation();
      }
      AnimationInit();
      display.clear();
      Serial.println(F("ACK RST"));
      return;
    }
    if (!strncmp("NXT", inputBuffer, 3)) {
      if (numLiveAnimations > 1) {
        skipToNextAnimation = true;
      }
      Serial.println(F("ACK NXT"));
      return;
    }
    if (!strncmp("DBG", inputBuffer, 3)) {
      Serial.print(F("animationInProgress: ")); Serial.println(animationInProgress);
      Serial.print(F("frameInProgress: ")); Serial.println(frameInProgress);
      Serial.print(F("frameInProgressLine: ")); Serial.println(frameInProgressLine);
      Serial.print(F("freeAnimations:"));
      for (AnimationIndex a = freeAnimations; a != kAnimationsSentinel; a = animations[a].next) {
        Serial.print(F(" ")); Serial.print(a);
      }
      Serial.println();
      Serial.print(F("liveAnimations:"));
      for (AnimationIndex a = liveAnimations; a != kAnimationsSentinel; a = animations[a].next) {
        Serial.print(F(" ")); Serial.print(a);
      }
      Serial.println();
      Serial.print(F("freeFrames:"));
      for (FrameIndex f = freeFrames; f != kFramesSentinel; f = frames[f].next) {
        Serial.print(F(" ")); Serial.print(f);
      }
      Serial.println();
      return;
    }
  }
  Serial.println(F("NAK CMD"));
}

void SerialInit() {
  Serial.begin(115200);
  Serial.setRxBufferSize(4*1024); // Enough to hold 30ms worth of serial data.
  Serial.println(F("Startup!"));
  bufP = inputBuffer;
  lineTooLong = false;
  animationInProgress = kAnimationsSentinel;
  frameInProgress = kFramesSentinel;
  frameInProgressLine = 0;  
}

void SerialUpdate() {
  if (Serial) {
    while (Serial.available()) {
      int c = Serial.read();
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
}

/*
 * And the usual Arduino song and dance.
 */
void setup() {
  pinMode(kOnboardLed0Pin, OUTPUT);
  pinMode(kOnboardLed1Pin, OUTPUT);
  pinMode(kLedMatrixDataPin, OUTPUT);
  pinMode(kLedMatrixPowerPin0, OUTPUT);
  pinMode(kLedMatrixPowerPin1, OUTPUT);
  pinMode(kTouch0Pin, INPUT);
  pinMode(kTouch1Pin, INPUT);
  pinMode(kTouch2Pin, INPUT);
  display.clear();
  FramesReset();
  AnimationsReset();
  SerialInit();
  AnimationInit();
}

void loop() {
  uint32_t loop_epoch = millis(); 
  animationClock = loop_epoch - animationEpoch;
  uint32_t next_loop = PowerUpdate();
  AnimationUpdate();
  SerialUpdate();
  
  // USB-C spec says the host can change the advertised current limit at any time,
  // and we have tSinkAdj(max) (60ms) to comply.
  // When we detect a CC value change, we also have to wait tRpValueChange(min) (10ms)
  // to make sure this is not a PD message (which cause, for our purposes, noise on the
  // CC line).
  // So we implement that by having the loop run at 30ms, and if we see a CC change,
  // we sample again in 15ms to make sure the value is stable, satisfying both timing
  // requirements.
  delay(loop_epoch + next_loop - loop_epoch);
}
