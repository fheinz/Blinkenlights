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
#include <avr/pgmspace.h>  // Needed to store stuff in Flash using PROGMEM
#include "FastLED.h"       // Fastled library to control the LEDs

#define NUM_COLS 16
#define NUM_LINES 16
#define NUM_LEDS (NUM_COLS * NUM_LINES)
#define MAX_FRAMES 32

#define DATA_PIN 1

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
  uint8_t pixels[NUM_LEDS*3];
  Duration duration;
  FrameIndex next;
} AnimationFrame;
#define FRAMES_SENTINEL ((FrameIndex)255)

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
#define ANIMATIONS_SENTINEL ((AnimationIndex)255)

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

#define ERROR_INVALID_FRAME_INDEX             1
#define ERROR_INVALID_ANIMATION_INDEX         2
#define ERROR_ENQUEUE_INVALID_ANIMATION       3
#define ERROR_ADD_FRAME_TO_INVALID_ANIMATION  4
#define ERROR_ADD_INVALID_FRAME_TO_ANIMATION  5
void CantHappen(const int err) {}


/*
 * String --> integer parsing utility functions
 */

#define ISDIGIT(c) ((c) >= '0' && (c) <= '9')
#define ISHEXDIGIT(c) ((c) >= 'A' && (c) <= 'F')

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
    if (!ISDIGIT(c))
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
    if (ISDIGIT(c)) {
      d = c -'0';
    } else if (ISHEXDIGIT(c)) {
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
      clear();
    }

    void clear() {
      FastLED.clear();
      FastLED.show();
    }
  
    void show(const AnimationFrame &f) {
      const uint8_t *pixels = f.pixels;
      FastLED.clear();
      for (int line = 0; line < NUM_LINES; line++) {
        for (int col = 0; col < NUM_COLS; col++) {
          int tgt_index = line * NUM_COLS + (line % 2 ? col : (NUM_COLS - 1) - col);
          leds[tgt_index] = CRGB(pixels[0], pixels[1], pixels[2]);  
          pixels += 3;
        }
      }
      FastLED.show();
    }
};


LedMatrix<NUM_LEDS, DATA_PIN> Display;

/*
 * The frames pool.
 */
AnimationFrame Frames[MAX_FRAMES];
FrameIndex FreeFrames;
uint8_t NumFreeFrames;

void FramesReset() {
    for (int i = 0; i < MAX_FRAMES-1; i++) {
    Frames[i].next = i+1;
  }
  Frames[MAX_FRAMES-1].next = FRAMES_SENTINEL;
  FreeFrames = (FrameIndex)0;
  NumFreeFrames = MAX_FRAMES;
}

FrameIndex FramesGetFrame() {
  FrameIndex f = FreeFrames;
  if (f != FRAMES_SENTINEL) {
    FreeFrames = Frames[f].next;
    Frames[f].next = FRAMES_SENTINEL;
    NumFreeFrames--;
  }
  return f;
}

void FramesFreeFrames(FrameIndex f) {
  while (f != FRAMES_SENTINEL) {
    if (f >= MAX_FRAMES) {
      CantHappen(ERROR_INVALID_FRAME_INDEX);
    }
    FrameIndex n = Frames[f].next;
    Frames[f].next = FreeFrames;
    FreeFrames = f;
    f = n;
    NumFreeFrames++;
  }
}

int FramesCountFrames(FrameIndex f) {
  int count = 0;
  
  while (f != FRAMES_SENTINEL) {
    count++;
    f = Frames[f].next;
  }
  return count;
}

/*
 * The animations pool.
 */
#define MAX_ANIMATIONS 16
Animation Animations[MAX_ANIMATIONS];
AnimationIndex FreeAnimations;
uint8_t NumFreeAnimations;
AnimationIndex LiveAnimations;
uint8_t NumLiveAnimations;

void AnimationsReset() {
  for (int i = 0; i < MAX_ANIMATIONS-1; i++) {
    Animations[i].next = i+1;
  }
  Animations[MAX_ANIMATIONS-1].next = ANIMATIONS_SENTINEL;
  FreeAnimations = (AnimationIndex)0;
  LiveAnimations = ANIMATIONS_SENTINEL;
  NumFreeAnimations = MAX_ANIMATIONS;
  NumLiveAnimations = 0;
}

AnimationIndex AnimationsGetAnimation() {
  AnimationIndex a = FreeAnimations;
  if (a != ANIMATIONS_SENTINEL) {
    FreeAnimations = Animations[a].next;
    NumFreeAnimations--;
    Animations[a].duration = 0;
    Animations[a].frames = FRAMES_SENTINEL;
    Animations[a].next = ANIMATIONS_SENTINEL;
  }
  return a;
}

void AnimationsFreeAnimation(AnimationIndex a) {
  while (a != ANIMATIONS_SENTINEL) {
    if (a >= MAX_ANIMATIONS) {
      CantHappen(ERROR_INVALID_ANIMATION_INDEX);
    }
    FramesFreeFrames(Animations[a].frames);
    Animations[a].frames = FRAMES_SENTINEL;
    AnimationIndex n = Animations[a].next;
    Animations[a].next = FreeAnimations;
    FreeAnimations = a;
    a = n;
    NumFreeAnimations++;
  }
}

/*
 * Building/enqueueing/destroying animations.
 */
void AnimationsAddFrame(AnimationIndex a, FrameIndex f) {
  if (a == ANIMATIONS_SENTINEL) {
    CantHappen(ERROR_ADD_FRAME_TO_INVALID_ANIMATION);
    return;
  }
  if (f == FRAMES_SENTINEL) {
    CantHappen(ERROR_ADD_INVALID_FRAME_TO_ANIMATION);
    return;
  }
  FrameIndex *cur = &Animations[a].frames;
  while (*cur != FRAMES_SENTINEL) {
    cur = &(Frames[*cur].next);
  }
  *cur = f;
  Frames[f].next = FRAMES_SENTINEL;
}

void AnimationsEnqueueAnimation(AnimationIndex a) {
  if (a == ANIMATIONS_SENTINEL) {
    CantHappen(ERROR_ENQUEUE_INVALID_ANIMATION);
    return;
  }
  AnimationIndex *cur = &LiveAnimations;
  while (*cur != ANIMATIONS_SENTINEL) {
    cur = &(Animations[*cur].next);
  }
  Animations[a].next = ANIMATIONS_SENTINEL;
  *cur = a;
  NumLiveAnimations++;
}

void AnimationsDequeueAnimation() {
  AnimationIndex cur = LiveAnimations;
  if (cur == ANIMATIONS_SENTINEL) 
    return;
  LiveAnimations = Animations[cur].next;
  Animations[cur].next = ANIMATIONS_SENTINEL;
  NumLiveAnimations--;
  if (LiveAnimations == ANIMATIONS_SENTINEL) {
    Display.clear();
  }
  AnimationsFreeAnimation(cur);
}

/*
 * The animation engine.
 */
FrameIndex NextFrame;             // Next frame to display
                                  // If FRAMES_SENTINEL ==> no active animation
uint32_t AnimationEpoch;          // Time at which the animation started
uint32_t AnimationClock;          // Time elapsed since AnimationEpoch
uint32_t FrameTransitionTime;     // Time after Epoch at which the a new frame is due
uint32_t AnimationTransitionTime; // Time after Epoch at which the current animation should end
bool SkipToNextAnimation;         // true ==> discard the current animation now

void AnimationUpdate() {
  if (NextFrame != FRAMES_SENTINEL && (AnimationClock >= AnimationTransitionTime || SkipToNextAnimation)) {
    AnimationsDequeueAnimation();
    NextFrame = FRAMES_SENTINEL;
  }
  SkipToNextAnimation = false;
  if (NextFrame == FRAMES_SENTINEL) {
    if (LiveAnimations != ANIMATIONS_SENTINEL) {
      NextFrame = Animations[LiveAnimations].frames;
      AnimationEpoch = AnimationClock + AnimationEpoch;
      FrameTransitionTime = AnimationClock = 0;
      AnimationTransitionTime = Animations[LiveAnimations].duration; 
    }
  }
  if (NextFrame != FRAMES_SENTINEL && AnimationClock >= FrameTransitionTime) {
    Display.show(Frames[NextFrame]);
    FrameTransitionTime = AnimationClock + Frames[NextFrame].duration;
    NextFrame = Frames[NextFrame].next;
    if (NextFrame == FRAMES_SENTINEL) {
      NextFrame = Animations[LiveAnimations].frames;
    }
  }
}

void AnimationInit() {
  NextFrame = FRAMES_SENTINEL;
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
char InputBuffer[BUFLEN];
char *BufP;
bool LineTooLong;

AnimationIndex AnimationInProgress;
FrameIndex FrameInProgress;
int FrameInProgressLine;

void CloseFrameConstruction() {
  if (FrameInProgress != FRAMES_SENTINEL) {
    AnimationsAddFrame(AnimationInProgress, FrameInProgress);
    FrameInProgress = FRAMES_SENTINEL;
    FrameInProgressLine = 0;
  }  
}

void CloseAnimationConstruction() {
  if (AnimationInProgress != ANIMATIONS_SENTINEL) {
    CloseFrameConstruction();
    AnimationsEnqueueAnimation(AnimationInProgress);
    AnimationInProgress = ANIMATIONS_SENTINEL;
  }
}

void ProcessCommand() {
  int l = BufP-InputBuffer;
  if (LineTooLong) {
    Serial.println(F("NAK LIN"));
    return;
  }
  if (l > 4) {
    if (!strncmp((const char *)F("CLC "), InputBuffer, 4)) {
      CRGB c;
      if (!ParseHex((uint8_t *)&c, InputBuffer+4, BufP)) {
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
    if (!strncmp("DIM ", InputBuffer, 4)) {
      uint32_t b;
      if (!ParseUInt32(&b, InputBuffer+4, BufP) || b > 255) {
        Serial.println(F("NAK DIM ARG"));
        return;
      }
      FastLED.setBrightness(b);
      Serial.print(F("ACK DIM "));
      Serial.print(b);
      return;
    }
    if (!strncmp("DTH ", InputBuffer, 4)) {
      if (l == 6 && !strncmp("ON", InputBuffer+4, 2)) {
        FastLED.setDither(BINARY_DITHER);       
        Serial.println(F("ACK DTH ON"));
        return;
      }
      if (l == 7 && !strncmp("OFF", InputBuffer+4, 3)) {
        FastLED.setDither(DISABLE_DITHER);
        Serial.println(F("ACK DTH OFF"));
        return;
      }
      Serial.println(F("NAK DTH ARG"));
      return;
    }
    if (!strncmp("RGB ", InputBuffer, 4)) {
      if (FrameInProgress == FRAMES_SENTINEL) {
        Serial.println(F("NAK RGB NFM"));
        return;
      }
      if (FrameInProgressLine >= NUM_LINES) {
        Serial.println(F("NAK RGB OFL"));
        return;        
      }
      if (l != 100 || !ParseHex(&(Frames[FrameInProgress].pixels[FrameInProgressLine*3*NUM_COLS]), InputBuffer+4, BufP)) {
        Serial.println(F("NAK RGB ARG"));
        return;
      }
      Serial.print(F("ACK RGB "));
      Serial.println(FrameInProgressLine);
      FrameInProgressLine++;
      return;
    }
    if (!strncmp("FRM ", InputBuffer, 4)) {
      Duration d;
      if (!ParseUInt32(&d, InputBuffer+4, BufP)) {
        Serial.println(F("NAK FRM ARG"));
        return;
      }
      CloseFrameConstruction();
      FrameInProgress = FramesGetFrame();    
      if (FrameInProgress == FRAMES_SENTINEL) {
        Serial.println(F("NAK FRM UFL"));
        return;
      }
      Frames[FrameInProgress].duration = d;
      Serial.print(F("ACK FRM "));
      Serial.println(d);
      return;   
    }
    if (!strncmp("ANM ", InputBuffer, 4)) {
      Duration d;
      if (!ParseUInt32(&d, InputBuffer+4, BufP)) {
        Serial.println(F("NAK ANM ARG"));
        return;
      }
      CloseAnimationConstruction();
      AnimationInProgress = AnimationsGetAnimation();
      if (AnimationInProgress == ANIMATIONS_SENTINEL) {
        Serial.println(F("NAK ANM UFL"));
        return;
      }
      Animations[AnimationInProgress].duration = d;
      Serial.print(F("ACK ANM "));
      Serial.println(d);
      return;   
    }
  }
  if (l == 3) {
    if (!strncmp("VER", InputBuffer, 3)) {
      Serial.println(F("ACK VER 1.0"));
      return;
    }
    if (!strncmp("QUE", InputBuffer, 3)) {
      AnimationIndex a = LiveAnimations;
      Serial.print(F("ACK QUE"));
      if (a != ANIMATIONS_SENTINEL) {
        Serial.print(F(" ("));
        Serial.print(Animations[a].duration-AnimationClock);
        Serial.print(F(", "));
        Serial.print(FramesCountFrames(Animations[a].frames));
        Serial.print(F(")"));
        a = Animations[a].next;
      }
      while (a != ANIMATIONS_SENTINEL) {
        Serial.print(F(" ("));
        Serial.print(Animations[a].duration);
        Serial.print(F(", "));
        Serial.print(FramesCountFrames(Animations[a].frames));
        Serial.print(F(")"));
        a = Animations[a].next;
      }
      Serial.println();
      return;
    }
    if (!strncmp("FRE", InputBuffer, 3)) {
      Serial.print(F("ACK FRE "));
      Serial.print(NumFreeAnimations);
      Serial.print(F(" "));
      Serial.println(NumFreeFrames);
      return;
    }
    if (!strncmp("DON", InputBuffer, 3)) {
      if (AnimationInProgress == ANIMATIONS_SENTINEL) {
        Serial.println(F("NAK DON NOA"));
        return;
      }
      CloseAnimationConstruction();
      Serial.println(F("ACK DON ANM"));
      return;
    }
    if (!strncmp("RST", InputBuffer, 3)) {
      CloseAnimationConstruction();
      while (LiveAnimations != ANIMATIONS_SENTINEL) {
        AnimationsDequeueAnimation();
      }
      AnimationInit();
      Display.clear();
      Serial.println(F("ACK RST"));
      return;
    }
    if (!strncmp("NXT", InputBuffer, 3)) {
      if (NumLiveAnimations > 1) {
        SkipToNextAnimation = true;
      }
      Serial.println(F("ACK NXT"));
      return;
    }
    if (!strncmp("DBG", InputBuffer, 3)) {
      Serial.print(F("AnimationInProgress: ")); Serial.println(AnimationInProgress);
      Serial.print(F("FrameInProgress: ")); Serial.println(FrameInProgress);
      Serial.print(F("FrameInProgressLine: ")); Serial.println(FrameInProgressLine);
      Serial.print(F("FreeAnimations:"));
      for (AnimationIndex a = FreeAnimations; a != ANIMATIONS_SENTINEL; a = Animations[a].next) {
        Serial.print(F(" ")); Serial.print(a);
      }
      Serial.println();
      Serial.print(F("LiveAnimations:"));
      for (AnimationIndex a = LiveAnimations; a != ANIMATIONS_SENTINEL; a = Animations[a].next) {
        Serial.print(F(" ")); Serial.print(a);
      }
      Serial.println();
      Serial.print(F("FreeFrames:"));
      for (FrameIndex f = FreeFrames; f != FRAMES_SENTINEL; f = Frames[f].next) {
        Serial.print(F(" ")); Serial.print(f);
      }
      Serial.println();
      return;
    }
  }
  Serial.println(F("NAK CMD"));
}

void SerialInit() {
  Serial.begin(9600);
  Serial.println(F("Startup!"));
  BufP = InputBuffer;
  LineTooLong = false;
  AnimationInProgress = ANIMATIONS_SENTINEL;
  FrameInProgress = FRAMES_SENTINEL;
  FrameInProgressLine = 0;  
}

void SerialUpdate() {
  if (Serial) {
    while (Serial.available() > 0) {
      int c = Serial.read();
      if (c == '\n') {
        if (BufP != InputBuffer) {
          ProcessCommand();
        }
        BufP = InputBuffer;
        LineTooLong = false;
      } else if (BufP - InputBuffer < sizeof(InputBuffer)) {
          *BufP++ = c;
      } else {
        LineTooLong = true;
      }
    }
  }
}

/*
 * And the usual Arduino song and dance.
 */
void setup() {
  FramesReset();
  AnimationsReset();
  SerialInit();
  AnimationInit();
}

void loop() { 
  AnimationClock = millis()-AnimationEpoch;
  AnimationUpdate();
  SerialUpdate();
}
