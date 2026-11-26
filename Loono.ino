// Loono — MIDI channel router + 4x16 step sequencer
// Board: Arduino Uno R4 Minima
// Behavior spec: see README.md (that doc is the contract, this follows it)
//
// Pins:
//   D2-D5   buttons row 1 (INPUT_PULLUP)
//   D6-D9   buttons row 2 (INPUT_PULLUP)
//   A0      tempo pot, 60-160 BPM
//   D0/RX1  MIDI in (opto circuit, Serial1 @ 31250 baud)
//   D1/TX1  MIDI out (Serial1 @ 31250 baud)
//   SDA/SCL OLED 128x64 (SSD1306, I2C)
//   D11     NeoPixel x4

#include <MIDI.h>
#include <Bounce2.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

MIDI_CREATE_INSTANCE(decltype(Serial1), Serial1, MIDI);

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define NEOPIXEL_PIN 11
#define NEOPIXEL_COUNT 4
Adafruit_NeoPixel pixels(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

#define POT_PIN A0      // BPM
#define LEN_POT_PIN A1  // pattern length

#define NUM_CH 4
#define NUM_STEPS 64  // every lane always stores 64; patLen picks how many play
#define MAX_CHORD 4

#define BPM_MIN 60
#define BPM_MAX 160
// Gate as a percentage of the step, split by tempo — tune by ear (README).
#define GATE_PCT_SLOW 70  // under 120 BPM
#define GATE_PCT_FAST 90  // 120 BPM and up
#define GATE_SPLIT_BPM 120

// Pot is sampled on an interval so the IIR actually smooths, then held in a
// deadband so ADC jitter can't flicker the BPM (which would also thrash the
// display and wobble the step length). Both are tuning values.
#define POT_INTERVAL_US 5000
#define POT_DEADBAND 4

// Full-frame OLED refresh is ~25ms at 400kHz, so it must never land on a step
// boundary (188ms at 160 BPM). Throttled, and skipped near the next step.
#define DISPLAY_MIN_INTERVAL_US 120000
#define DISPLAY_STEP_GUARD_US 40000

// How long an audition of a step rings when you arrive on it, and how long a
// "cleared" confirmation stays on screen. Both tuning values.
#define PREVIEW_US 350000
#define TOAST_US 1000000

enum StepType : uint8_t { STEP_REST, STEP_CHORD, STEP_TIE };

struct Step {
  uint8_t type;
  uint8_t count;
  uint8_t notes[MAX_CHORD];
  uint8_t vels[MAX_CHORD];
};

struct Channel {
  bool sequencer;
  Step steps[NUM_STEPS];
  uint8_t soundCount;              // notes currently ringing from this lane
  uint8_t soundNotes[MAX_CHORD];
  bool gateArmed;                  // false = note rides on into a tie
  uint32_t gateDeadline;           // micros
};

Channel chans[NUM_CH];
uint8_t active = 0;
bool editing = false;
bool playing = false;
uint8_t playStep = 0;
uint8_t editCursor = 0;
// How many of the 64 stored steps play, shared by all lanes like the clock.
// Shortening never erases: lengthen again and the steps come back.
uint8_t patLen = 16;
uint32_t stepStart = 0;  // micros
uint16_t bpm = 100;

// Live-key tracking, used only while editing: a chord commits once every note
// it recorded has been released. Deliberately independent of any other held
// key, so a 5th note or a phantom NoteOn can't wedge recording.
bool keyHeld[128];
uint8_t recNotes[MAX_CHORD];
uint8_t recVels[MAX_CHORD];
uint8_t recCount = 0;

const uint8_t BUTTON_PINS[] = {2, 3, 4, 5, 6, 7, 8, 9};
const uint8_t NUM_BUTTONS = sizeof(BUTTON_PINS) / sizeof(BUTTON_PINS[0]);
Bounce buttons[NUM_BUTTONS];

bool displayDirty = true;
uint32_t lastDraw = 0;
const char *toastMsg = nullptr;  // brief confirmation line, e.g. "Step cleared"
uint32_t toastUntil = 0;
uint32_t ledCache[NUM_CH] = {1, 1, 1, 1};  // impossible value forces first show

struct Pot {
  uint8_t pin;
  uint32_t accum;         // IIR, 8x the filtered value
  uint16_t lastFiltered;
};
Pot bpmPot = {POT_PIN, 0, 0};
Pot lenPot = {LEN_POT_PIN, 0, 0};

const char *NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// One step = one 1/8 note. Microseconds, not millis: at 160 BPM a step is
// 187.5ms, and truncating that to 187ms runs the tempo fast and drifts the grid.
uint32_t stepUs() { return 30000000UL / bpm; }

uint32_t gateUs() {
  uint8_t pct = (bpm >= GATE_SPLIT_BPM) ? GATE_PCT_FAST : GATE_PCT_SLOW;
  return stepUs() / 100 * pct;
}

// A tie at step 1 has nothing before it to sustain, so it plays as a rest.
uint8_t effectiveType(uint8_t c, uint8_t idx) {
  uint8_t t = chans[c].steps[idx].type;
  if (t == STEP_TIE && idx == 0) return STEP_REST;
  return t;
}

void stopSounding(uint8_t c) {
  for (uint8_t i = 0; i < chans[c].soundCount; i++) {
    MIDI.sendNoteOff(chans[c].soundNotes[i], 0, c + 1);
  }
  chans[c].soundCount = 0;
  chans[c].gateArmed = false;
}

void setChannelMode(uint8_t c, bool sequencer) {
  if (chans[c].sequencer == sequencer) return;
  chans[c].sequencer = sequencer;
  if (sequencer) {
    // A key held across this toggle would never get its note off forwarded,
    // because sequencer channels don't pass live notes. Clear it now.
    MIDI.sendControlChange(123, 0, c + 1);
  } else {
    stopSounding(c);  // nothing left stranded without a lane to end it
    if (c == active) editing = false;
  }
  displayDirty = true;
}

void setActive(uint8_t c) {
  active = c;
  // Clears a note stranded on this channel by the hold-and-switch quirk.
  // Skipped for sequencer lanes, which may legitimately be sustaining a tie.
  if (!chans[c].sequencer) MIDI.sendControlChange(123, 0, c + 1);
  displayDirty = true;
}

void stopClock() {
  playing = false;
  for (uint8_t c = 0; c < NUM_CH; c++) stopSounding(c);
  displayDirty = true;
}

void fireStep() {
  uint8_t nextIdx = (playStep + 1) % patLen;
  for (uint8_t c = 0; c < NUM_CH; c++) {
    if (!chans[c].sequencer) continue;
    Step &s = chans[c].steps[playStep];
    uint8_t type = effectiveType(c, playStep);

    // A following tie must not be cut off by this step's gate.
    bool nextTies = (effectiveType(c, nextIdx) == STEP_TIE);

    if (type == STEP_CHORD) {
      stopSounding(c);
      for (uint8_t i = 0; i < s.count; i++) {
        MIDI.sendNoteOn(s.notes[i], s.vels[i], c + 1);
        chans[c].soundNotes[i] = s.notes[i];
      }
      chans[c].soundCount = s.count;
      if (s.count && !nextTies) {
        chans[c].gateDeadline = stepStart + gateUs();
        chans[c].gateArmed = true;
      }
    } else if (type == STEP_TIE) {
      if (chans[c].soundCount && !nextTies) {
        chans[c].gateDeadline = stepStart + gateUs();
        chans[c].gateArmed = true;
      }
    } else {
      stopSounding(c);
    }
  }
}

void startClock() {
  playing = true;
  stepStart = micros();
  fireStep();
  displayDirty = true;
}

void advanceClock(uint32_t now) {
  uint32_t su = stepUs();
  if (now - stepStart < su) return;
  stepStart += su;  // accumulate, so the grid doesn't drift with loop jitter
  if (now - stepStart >= su * 2) stepStart = now;  // resync if we fell behind
  playStep = (playStep + 1) % patLen;  // shortening lands back in the window here
  fireStep();
  displayDirty = true;
}

void updateGates(uint32_t now) {
  for (uint8_t c = 0; c < NUM_CH; c++) {
    if (chans[c].gateArmed && (int32_t)(now - chans[c].gateDeadline) >= 0) {
      stopSounding(c);
    }
  }
}

void toast(const char *msg) {
  toastMsg = msg;
  toastUntil = micros() + TOAST_US;
  displayDirty = true;
}

// Arriving on a step sounds whatever it holds, so the arrows double as a way to
// audition a prepared lane and stop on the step you want to replace.
void previewStep() {
  Step &s = chans[active].steps[editCursor];
  stopSounding(active);
  if (s.type != STEP_CHORD || !s.count) return;
  for (uint8_t i = 0; i < s.count; i++) {
    MIDI.sendNoteOn(s.notes[i], s.vels[i], active + 1);
    chans[active].soundNotes[i] = s.notes[i];
  }
  chans[active].soundCount = s.count;
  chans[active].gateDeadline = micros() + PREVIEW_US;
  chans[active].gateArmed = true;
}

// The length pot is live in every mode, mid-playback included. Playback wraps
// into range on its own at the next step; the edit cursor has to be pulled in.
void setPatLen(uint8_t n) {
  if (n == patLen) return;
  patLen = n;
  if (editCursor >= patLen) editCursor = patLen - 1;
  displayDirty = true;
}

void moveCursor(int8_t delta) {
  editCursor = (editCursor + patLen + delta) % patLen;
  previewStep();
  displayDirty = true;
}

void writeStep(uint8_t type) {
  Step &s = chans[active].steps[editCursor];
  s.type = type;
  s.count = 0;
  moveCursor(1);
}

void commitChord() {
  Step &s = chans[active].steps[editCursor];
  s.type = STEP_CHORD;
  s.count = recCount;
  for (uint8_t i = 0; i < recCount; i++) {
    s.notes[i] = recNotes[i];
    s.vels[i] = recVels[i];
  }
  recCount = 0;
  moveCursor(1);
}

void clearChannel(uint8_t c) {
  for (uint8_t i = 0; i < NUM_STEPS; i++) {
    chans[c].steps[i].type = STEP_REST;
    chans[c].steps[i].count = 0;
  }
  stopSounding(c);
  displayDirty = true;
}

void enterEdit() {
  stopClock();
  editing = true;
  editCursor = 0;
  recCount = 0;
  memset(keyHeld, 0, sizeof(keyHeld));  // drop stale holds from before edit
  previewStep();
  displayDirty = true;
}

void noteOn(uint8_t note, uint8_t vel) {
  if (editing) {
    if (!keyHeld[note]) {
      if (recCount < MAX_CHORD) {  // first 4 win, extras only sound
        recNotes[recCount] = note;
        recVels[recCount] = vel;  // chords keep the velocity they were played at
        recCount++;
      }
      keyHeld[note] = true;
      displayDirty = true;
    }
    MIDI.sendNoteOn(note, vel, active + 1);  // audition: hear what you record
  } else if (!chans[active].sequencer) {
    MIDI.sendNoteOn(note, vel, active + 1);
  }
  // sequencer mode, not editing: a channel is sequenced or played, never both
}

void noteOff(uint8_t note) {
  if (editing) {
    MIDI.sendNoteOff(note, 0, active + 1);  // end the audition
    if (!keyHeld[note]) return;
    keyHeld[note] = false;
    if (!recCount) return;
    for (uint8_t i = 0; i < recCount; i++) {
      if (keyHeld[recNotes[i]]) return;  // chord still being held
    }
    commitChord();
  } else if (!chans[active].sequencer) {
    // Uses whichever channel is active right now — this is what lets a held
    // note keep sounding on the channel you switched away from.
    MIDI.sendNoteOff(note, 0, active + 1);
  }
}

void readMidi() {
  while (MIDI.read()) {
    midi::MidiType type = MIDI.getType();
    uint8_t d1 = MIDI.getData1();
    uint8_t d2 = MIDI.getData2();
    if (type == midi::NoteOn && d2 > 0) {
      noteOn(d1, d2);
    } else if (type == midi::NoteOff || (type == midi::NoteOn && d2 == 0)) {
      noteOff(d1);
    } else {
      // CC, bend, program change, aftertouch keep their incoming channel;
      // the library routes real-time (clock/start/stop) on its own.
      MIDI.send(type, d1, d2, MIDI.getChannel());
    }
  }
}

void handleButton(uint8_t i) {
  // Clear-all is live whenever the active lane is a sequencer, running or not,
  // so a bad pattern can be wiped without stopping.
  if (i == 7 && chans[active].sequencer) {
    clearChannel(active);
    toast("All cleared");
    return;
  }

  if (editing) {
    switch (i) {
      case 0: moveCursor(-1); break;
      case 1: moveCursor(1); break;
      case 2: writeStep(STEP_REST); break;   // skip
      case 3: writeStep(STEP_TIE); break;    // tie
      case 5: editing = false; stopSounding(active); displayDirty = true; break;
      case 6: chans[active].steps[editCursor].type = STEP_REST;
              chans[active].steps[editCursor].count = 0;
              stopSounding(active);
              toast("Step cleared");
              break;
    }
    return;
  }

  if (i < NUM_CH) {
    if (i == active) {
      setChannelMode(active, !chans[active].sequencer);
    } else {
      setActive(i);
    }
  } else if (i == 4) {
    playing ? stopClock() : startClock();  // global, live in every mode
  } else if (i == 5 && chans[active].sequencer) {
    enterEdit();
  }
}

// Primed from the real wiper so the first reading isn't an eighth of it.
void primePot(Pot &p) {
  p.accum = (uint32_t)analogRead(p.pin) << 3;
  p.lastFiltered = p.accum >> 3;
}

// true with *out filled when this pot moved past the deadband.
bool potMoved(Pot &p, uint16_t &out) {
  p.accum = p.accum - (p.accum >> 3) + analogRead(p.pin);
  uint16_t filtered = p.accum >> 3;
  int16_t drift = (int16_t)filtered - (int16_t)p.lastFiltered;
  if (drift < POT_DEADBAND && drift > -POT_DEADBAND) return false;
  p.lastFiltered = filtered;
  out = filtered;
  return true;
}

void readPots(uint32_t now) {
  static uint32_t lastSample = 0;
  if (now - lastSample < POT_INTERVAL_US) return;
  lastSample = now;

  uint16_t v;
  if (potMoved(bpmPot, v)) {
    uint16_t next = map(v, 0, 1023, BPM_MIN, BPM_MAX);
    if (next != bpm) {
      bpm = next;
      displayDirty = true;
    }
  }
  // 1024 counts over 64 lengths is 16 per step, well clear of the deadband.
  if (potMoved(lenPot, v)) setPatLen(map(v, 0, 1023, 1, NUM_STEPS));
}

void updateLeds() {
  bool changed = false;
  for (uint8_t c = 0; c < NUM_CH; c++) {
    uint32_t color;
    if (c == active) {
      color = editing                ? pixels.Color(24, 8, 0)   // orange
            : chans[c].sequencer     ? pixels.Color(12, 0, 24)  // violet
                                     : pixels.Color(0, 20, 0);  // green
    } else {
      color = chans[c].sequencer ? pixels.Color(1, 0, 3) : 0; // dim violet / off
    }
    if (color != ledCache[c]) {
      ledCache[c] = color;
      pixels.setPixelColor(c, color);
      changed = true;
    }
  }
  if (changed) pixels.show();
}

void printStepNum(uint8_t n) {
  if (n < 10) display.print('0');  // always two digits: 01 .. 64
  display.print(n);
}

void printNoteName(uint8_t note) {
  display.print(NOTE_NAMES[note % 12]);
  display.print((int)(note / 12) - 1);
}

// ponytail: full-buffer redraw (~25ms over I2C); upgrade to partial window
// writes if the throttle + step guard below ever proves too coarse.
void updateDisplay(uint32_t now) {
  if (!displayDirty) return;
  if (now - lastDraw < DISPLAY_MIN_INTERVAL_US) return;
  if (playing) {
    uint32_t elapsed = now - stepStart;
    uint32_t su = stepUs();
    if (su > elapsed && su - elapsed < DISPLAY_STEP_GUARD_US) return;  // step imminent
  }

  display.clearDisplay();

  display.setTextSize(1);
  if (chans[active].sequencer) {  // in standard the big line already says it
    display.setCursor(0, 0);
    display.print("CH");
    display.print(active + 1);
    display.print(" L");
    display.print(patLen);
  }

  display.setCursor(SCREEN_WIDTH - 46, 0);
  display.print(bpm);
  display.print(" BPM");

  // Mode is read off the LED colors, so the screen carries transport + step.
  display.setTextSize(2);
  display.setCursor(0, 20);
  if (editing) {
    display.print("Edit ");
    printStepNum(editCursor + 1);
  } else if (chans[active].sequencer) {
    display.print(playing ? "Play. " : "Pause ");
    printStepNum(playStep + 1);
  } else {
    display.print("Loono ");  // standard mode home screen: device + channel
    printStepNum(active + 1);
  }

  if (toastMsg) {
    display.setTextSize(1);
    display.setCursor(0, 50);
    display.print(toastMsg);
  } else if (editing) {
    // Held keys while building a chord, otherwise what the step already holds.
    display.setTextSize(1);
    display.setCursor(0, 50);
    if (recCount) {
      for (uint8_t i = 0; i < recCount; i++) {
        if (i) display.print(" ");
        printNoteName(recNotes[i]);
      }
    } else {
      Step &s = chans[active].steps[editCursor];
      if (s.type == STEP_TIE) {
        display.print("Tie");
      } else if (s.type == STEP_CHORD && s.count) {
        for (uint8_t i = 0; i < s.count; i++) {
          if (i) display.print(" ");
          printNoteName(s.notes[i]);
        }
      } else {
        display.print("Skip");
      }
    }
  }

  display.display();
  lastDraw = now;
  displayDirty = false;
}

// Build with -DLOONO_SELFTEST to assert the step/gate/tie logic over USB serial.
#ifdef LOONO_SELFTEST
uint8_t stFails = 0;
void stCheck(const char *name, long got, long want) {
  if (got != want) {
    stFails++;
    Serial.print("FAIL ");
  } else {
    Serial.print("ok   ");
  }
  Serial.print(name);
  Serial.print(" got=");
  Serial.print(got);
  Serial.print(" want=");
  Serial.println(want);
}

void selftest() {
  stFails = 0;

  bpm = 60;  stCheck("stepUs@60 is 1/8 note", stepUs(), 500000);
  bpm = 120; stCheck("stepUs@120", stepUs(), 250000);
  bpm = 160; stCheck("stepUs@160 keeps .5ms", stepUs(), 187500);

  bpm = 100; stCheck("gate@100 slow 70pct", gateUs(), 210000);
  bpm = 120; stCheck("gate@120 fast 90pct", gateUs(), 225000);

  clearChannel(0);
  stCheck("cleared step is rest", chans[0].steps[3].type, STEP_REST);

  chans[0].steps[0].type = STEP_TIE;
  chans[0].steps[5].type = STEP_TIE;
  stCheck("tie at step 1 is rest", effectiveType(0, 0), STEP_REST);
  stCheck("tie mid-lane ties", effectiveType(0, 5), STEP_TIE);
  clearChannel(0);

  // A chord followed by a tie must not schedule a gate cut, or the tie is silent.
  chans[0].sequencer = true;
  chans[0].steps[2].type = STEP_CHORD;
  chans[0].steps[2].count = 1;
  chans[0].steps[2].notes[0] = 60;
  chans[0].steps[3].type = STEP_TIE;
  playStep = 2;
  stepStart = micros();
  fireStep();
  stCheck("chord before tie rides", chans[0].gateArmed, 0);
  stCheck("chord is sounding", chans[0].soundCount, 1);

  playStep = 3;  // the tie itself ends the chain and takes the gate
  stepStart = micros();
  fireStep();
  stCheck("tie still sounding", chans[0].soundCount, 1);
  stCheck("tie arms the cut", chans[0].gateArmed, 1);

  playStep = 4;  // rest clears it
  stepStart = micros();
  fireStep();
  stCheck("rest silences lane", chans[0].soundCount, 0);

  // An empty chord step must not arm a gate with nothing to cut.
  clearChannel(0);
  chans[0].steps[1].type = STEP_CHORD;
  chans[0].steps[1].count = 0;
  playStep = 1;
  stepStart = micros();
  fireStep();
  stCheck("empty chord arms nothing", chans[0].gateArmed, 0);

  // Recording: 5th note ignored, chord commits only when its own notes drop.
  clearChannel(0);
  editing = true;
  editCursor = 0;
  recCount = 0;
  memset(keyHeld, 0, sizeof(keyHeld));
  noteOn(60, 100); noteOn(64, 90); noteOn(67, 80); noteOn(72, 70);
  noteOn(76, 60);  // 5th, must be dropped
  stCheck("chord caps at 4", recCount, 4);
  noteOff(60); noteOff(64); noteOff(67);
  stCheck("no commit while held", chans[0].steps[0].type, STEP_REST);
  noteOff(72);
  stCheck("commits on last release", chans[0].steps[0].type, STEP_CHORD);
  stCheck("commit kept 4 notes", chans[0].steps[0].count, 4);
  stCheck("commit kept velocity", chans[0].steps[0].vels[1], 90);
  stCheck("cursor advanced", editCursor, 1);
  noteOff(76);  // stray release of the ignored note must not re-commit
  stCheck("stray release is inert", editCursor, 1);

  // Length windows playback and the cursor without erasing anything past it.
  clearChannel(0);
  patLen = 16;
  editCursor = 15;
  chans[0].steps[20].type = STEP_CHORD;  // outside the window
  chans[0].steps[20].count = 1;
  chans[0].steps[20].notes[0] = 72;
  moveCursor(1);
  stCheck("cursor wraps at length", editCursor, 0);
  setPatLen(8);
  editCursor = 12;
  setPatLen(8);          // no-op, cursor must stay put
  stCheck("same length leaves cursor", editCursor, 12);
  patLen = 16;
  editCursor = 12;
  setPatLen(8);
  stCheck("shrink pulls cursor in", editCursor, 7);
  playStep = 20;
  stepStart = micros();
  advanceClock(micros() + stepUs());
  stCheck("playback wraps into window", playStep < patLen, 1);
  setPatLen(32);
  stCheck("step past old length survives", chans[0].steps[20].count, 1);
  stCheck("its note survives", chans[0].steps[20].notes[0], 72);
  patLen = 16;

  editing = false;
  clearChannel(0);
  chans[0].sequencer = false;
  playStep = 0;
  recCount = 0;
  memset(keyHeld, 0, sizeof(keyHeld));
  bpm = 100;

  Serial.print("LOONO SELFTEST ");
  Serial.println(stFails ? "FAILED" : "PASSED");
}
#endif

void setup() {
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    buttons[i].attach(BUTTON_PINS[i], INPUT_PULLUP);
    buttons[i].interval(5);
  }

  MIDI.begin(MIDI_CHANNEL_OMNI);  // before clearChannel, which sends note offs
  MIDI.turnThruOff();

  for (uint8_t c = 0; c < NUM_CH; c++) {
    chans[c].sequencer = false;  // boot: all standard, ch1 active, clock stopped
    clearChannel(c);
  }

  pixels.begin();

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  Wire.setClock(400000);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(3);
  int16_t bx, by;
  uint16_t bw, bh;
  display.getTextBounds("Loono", 0, 0, &bx, &by, &bw, &bh);
  display.setCursor((SCREEN_WIDTH - bw) / 2 - bx, (SCREEN_HEIGHT - bh) / 2 - by);
  display.print("Loono");
  display.display();
  delay(2000);  // splash; the only delay() in the sketch

  primePot(bpmPot);
  primePot(lenPot);
  bpm = map(bpmPot.lastFiltered, 0, 1023, BPM_MIN, BPM_MAX);
  patLen = map(lenPot.lastFiltered, 0, 1023, 1, NUM_STEPS);
  displayDirty = true;

#ifdef LOONO_SELFTEST
  Serial.begin(9600);  // selftest build re-runs from loop() so it can't be missed
#endif
}

void loop() {
#ifdef LOONO_SELFTEST
  static uint32_t lastRun = 0;
  if (millis() - lastRun > 3000) {
    lastRun = millis();
    selftest();
  }
  return;  // test build does not run normal operation
#endif

  readMidi();  // MIDI first, always — nothing below it may block

  uint32_t now = micros();
  if (playing) advanceClock(now);
  updateGates(now);

  if (toastMsg && (int32_t)(now - toastUntil) >= 0) {
    toastMsg = nullptr;
    displayDirty = true;
  }

  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    buttons[i].update();
    if (buttons[i].fell()) handleButton(i);
  }

  readPots(now);
  updateLeds();
  updateDisplay(now);
}
