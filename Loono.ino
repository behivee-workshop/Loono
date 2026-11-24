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

#define POT_PIN A0

#define NUM_CH 4
#define NUM_STEPS 16
#define MAX_CHORD 4

#define BPM_MIN 60
#define BPM_MAX 160
// Gate as a percentage of the step, split by tempo — tune by ear (README).
#define GATE_PCT_SLOW 70  // under 120 BPM
#define GATE_PCT_FAST 90  // 120 BPM and up
#define GATE_SPLIT_BPM 120

// Full-frame OLED refresh is ~25ms at 400kHz, so it must never land on a step
// boundary (94ms at 160 BPM). Throttled, and skipped near the next step.
#define DISPLAY_MIN_INTERVAL 120
#define DISPLAY_STEP_GUARD 40

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
  uint32_t gateDeadline;           // 0 = no cut scheduled (note rides into a tie)
};

Channel chans[NUM_CH];
uint8_t active = 0;
bool editing = false;
bool playing = false;
uint8_t playStep = 0;
uint8_t editCursor = 0;
uint32_t stepStart = 0;
uint16_t bpm = 100;

// Live-key tracking. Recording keeps the first MAX_CHORD notes of a press;
// the chord commits when the last key comes up.
bool keyHeld[128];
uint8_t heldCount = 0;
uint8_t recNotes[MAX_CHORD];
uint8_t recVels[MAX_CHORD];
uint8_t recCount = 0;

const uint8_t BUTTON_PINS[] = {2, 3, 4, 5, 6, 7, 8, 9};
const uint8_t NUM_BUTTONS = sizeof(BUTTON_PINS) / sizeof(BUTTON_PINS[0]);
Bounce buttons[NUM_BUTTONS];

bool displayDirty = true;
uint32_t lastDraw = 0;
uint32_t ledCache[NUM_CH] = {1, 1, 1, 1};  // impossible value forces first show
uint32_t potAccum = 0;

const char *NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

uint32_t stepMs() { return 60000UL / bpm / 4; }

uint32_t gateMs() {
  uint8_t pct = (bpm >= GATE_SPLIT_BPM) ? GATE_PCT_FAST : GATE_PCT_SLOW;
  return stepMs() * pct / 100;
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
  chans[c].gateDeadline = 0;
}

void setChannelMode(uint8_t c, bool sequencer) {
  if (chans[c].sequencer == sequencer) return;
  chans[c].sequencer = sequencer;
  if (!sequencer) {
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
  uint8_t nextIdx = (playStep + 1) % NUM_STEPS;
  for (uint8_t c = 0; c < NUM_CH; c++) {
    if (!chans[c].sequencer) continue;
    Step &s = chans[c].steps[playStep];
    uint8_t type = effectiveType(c, playStep);

    // A following tie must not be cut off by this step's gate.
    bool nextTies = (effectiveType(c, nextIdx) == STEP_TIE);
    uint32_t deadline = nextTies ? 0 : stepStart + gateMs();

    if (type == STEP_CHORD) {
      stopSounding(c);
      for (uint8_t i = 0; i < s.count; i++) {
        MIDI.sendNoteOn(s.notes[i], s.vels[i], c + 1);
        chans[c].soundNotes[i] = s.notes[i];
      }
      chans[c].soundCount = s.count;
      chans[c].gateDeadline = deadline;
    } else if (type == STEP_TIE) {
      if (chans[c].soundCount) chans[c].gateDeadline = deadline;
    } else {
      stopSounding(c);
    }
  }
}

void startClock() {
  playing = true;
  stepStart = millis();
  fireStep();
  displayDirty = true;
}

void advanceClock(uint32_t now) {
  uint32_t sm = stepMs();
  if (now - stepStart < sm) return;
  stepStart += sm;
  if (now - stepStart >= sm * 2) stepStart = now;  // resync if we fell behind
  playStep = (playStep + 1) % NUM_STEPS;
  fireStep();
  displayDirty = true;
}

void updateGates(uint32_t now) {
  for (uint8_t c = 0; c < NUM_CH; c++) {
    if (chans[c].gateDeadline && (int32_t)(now - chans[c].gateDeadline) >= 0) {
      stopSounding(c);
    }
  }
}

void writeStep(uint8_t type) {
  Step &s = chans[active].steps[editCursor];
  s.type = type;
  s.count = 0;
  editCursor = (editCursor + 1) % NUM_STEPS;
  displayDirty = true;
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
  editCursor = (editCursor + 1) % NUM_STEPS;
  displayDirty = true;
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
  displayDirty = true;
}

void noteOn(uint8_t note, uint8_t vel) {
  if (keyHeld[note]) return;
  keyHeld[note] = true;
  heldCount++;

  if (editing) {
    if (recCount < MAX_CHORD) {  // first 4 pressed win, extras ignored
      recNotes[recCount] = note;
      recVels[recCount] = vel;
      recCount++;
      displayDirty = true;
    }
  } else if (!chans[active].sequencer) {
    MIDI.sendNoteOn(note, vel, active + 1);
  }
  // sequencer mode, not editing: a channel is sequenced or played, never both
}

void noteOff(uint8_t note) {
  if (!keyHeld[note]) return;
  keyHeld[note] = false;
  if (heldCount) heldCount--;

  if (editing) {
    if (heldCount == 0 && recCount > 0) commitChord();
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
      MIDI.send(type, d1, d2, MIDI.getChannel());  // CC, bend, clock: untouched
    }
  }
}

void handleButton(uint8_t i) {
  if (editing) {
    switch (i) {
      case 0: editCursor = (editCursor + NUM_STEPS - 1) % NUM_STEPS; displayDirty = true; break;
      case 1: editCursor = (editCursor + 1) % NUM_STEPS; displayDirty = true; break;
      case 2: writeStep(STEP_REST); break;   // skip
      case 3: writeStep(STEP_TIE); break;    // tie
      case 5: editing = false; displayDirty = true; break;
      case 6: chans[active].steps[editCursor].type = STEP_REST;
              chans[active].steps[editCursor].count = 0;
              displayDirty = true; break;
      case 7: clearChannel(active); break;
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

void readPot() {
  potAccum = potAccum - (potAccum >> 3) + analogRead(POT_PIN);
  uint16_t next = map(potAccum >> 3, 0, 1023, BPM_MIN, BPM_MAX);
  if (next != bpm) {
    bpm = next;
    displayDirty = true;
  }
}

void updateLeds() {
  for (uint8_t c = 0; c < NUM_CH; c++) {
    uint32_t color;
    if (c == active) color = pixels.Color(0, 20, 0);          // solid green
    else if (chans[c].sequencer) color = pixels.Color(5, 5, 5);  // dim white
    else color = 0;
    if (color != ledCache[c]) {
      ledCache[c] = color;
      pixels.setPixelColor(c, color);
      pixels.show();
    }
  }
}

void printNoteName(uint8_t note) {
  display.print(NOTE_NAMES[note % 12]);
  display.print((int)(note / 12) - 1);
}

// ponytail: full-buffer redraw (~25ms over I2C); upgrade to partial window
// writes if the throttle + step guard below ever proves too coarse.
void updateDisplay(uint32_t now) {
  if (!displayDirty) return;
  if (now - lastDraw < DISPLAY_MIN_INTERVAL) return;
  if (playing) {
    uint32_t elapsed = now - stepStart;
    uint32_t sm = stepMs();
    if (sm > elapsed && sm - elapsed < DISPLAY_STEP_GUARD) return;  // step imminent
  }

  display.clearDisplay();

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("CH");
  display.print(active + 1);

  display.setCursor(SCREEN_WIDTH - 46, 0);
  display.print(bpm);
  display.print(" BPM");

  display.setTextSize(2);
  display.setCursor(0, 20);
  if (editing) display.print("EDIT");
  else if (chans[active].sequencer) display.print(playing ? "PLAY" : "SEQ");
  else display.print("STD");

  display.setTextSize(1);
  display.setCursor(0, 42);
  if (editing) {
    display.print("step ");
    display.print(editCursor + 1);
    display.print("/");
    display.print(NUM_STEPS);
  } else if (playing) {
    display.print("step ");
    display.print(playStep + 1);
    display.print("/");
    display.print(NUM_STEPS);
  }

  if (editing && recCount) {
    display.setCursor(0, 54);
    for (uint8_t i = 0; i < recCount; i++) {
      if (i) display.print(" ");
      printNoteName(recNotes[i]);
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

  bpm = 60;  stCheck("stepMs@60", stepMs(), 250);
  bpm = 120; stCheck("stepMs@120", stepMs(), 125);
  bpm = 160; stCheck("stepMs@160", stepMs(), 93);

  bpm = 100; stCheck("gate@100 slow 70pct", gateMs(), 105);
  bpm = 120; stCheck("gate@120 fast 90pct", gateMs(), 112);

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
  stepStart = millis();
  fireStep();
  stCheck("chord before tie rides (no gate)", chans[0].gateDeadline, 0);
  stCheck("chord is sounding", chans[0].soundCount, 1);

  playStep = 3;  // the tie itself ends the chain and takes the gate
  stepStart = millis();
  fireStep();
  stCheck("tie still sounding", chans[0].soundCount, 1);
  stCheck("tie schedules the cut", chans[0].gateDeadline != 0, 1);

  playStep = 4;  // rest clears it
  stepStart = millis();
  fireStep();
  stCheck("rest silences lane", chans[0].soundCount, 0);

  clearChannel(0);
  chans[0].sequencer = false;
  playStep = 0;
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
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Loono");
  display.display();
  delay(2000);  // splash; the only delay() in the sketch

  potAccum = (uint32_t)analogRead(POT_PIN) << 3;
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

  uint32_t now = millis();
  if (playing) advanceClock(now);
  updateGates(now);

  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    buttons[i].update();
    if (buttons[i].fell()) handleButton(i);
  }

  readPot();
  updateLeds();
  updateDisplay(now);
}
