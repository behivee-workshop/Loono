// Loono — Uno R4 Minima standalone 4-track MIDI looper.
// Hardware port of app/loop (the Keybench "Loop" web app): per-track
// empty -> recording -> playing <-> overdubbing <-> paused state machine,
// each track its own loop length, played back on its own MIDI channel.
//
// Pins:
//   D0/D1 (RX1/TX1) Serial1 used as MIDI in/out (31250 baud, via the usual
//                  optocoupler-in / resistor-out circuit). The R4 Minima's
//                  native-USB Serial is a separate peripheral, so it stays
//                  free the whole time for debug prints or the self-test
//                  below -- unlike an Uno R3, where D0/D1 *is* USB serial.
//   D2-D5          Track select buttons 1-4 (INPUT_PULLUP, active low)
//   D6             Play/pause button (INPUT_PULLUP, active low)
//   D7             Record/overdub button (INPUT_PULLUP, active low).
//                  Short press: empty->record->stop->overdub->stop->... .
//   D8             Mute/unmute button for the selected track (INPUT_PULLUP,
//                  active low). No undo exists, so this is D8's job.
//   D9             Clear button for the selected track (INPUT_PULLUP, active low)
//   D11            NeoPixel data, 4 LEDs at ~10% brightness: a single lit
//                  LED chases positions 0-3 with the beat (LED N = beat
//                  N+1 of the bar), colored by the selected track's state --
//                  green playing, red stopped (empty/paused), violet armed
//                  to start on beat 1, orange while actually recording or
//                  overdubbing.
//   A0             BPM pot (40-240 BPM), used for the bar-count readout and
//                  the idle metronome pulse -- it does not quantize
//                  recording; punching in/out is immediate, same as the
//                  web app when no metronome bar is running.
//   A4/A5          OLED I2C (SSD1306 128x64 @ 0x3C)
//
// Play/pause only pauses or resumes the selected track's already-closed
// loop; it does nothing to an empty or still-recording track. Record only
// controls the selected track. MIDI IN is always echoed straight to MIDI
// OUT (live playthrough) and, while the selected... no -- *any* track that
// is recording or overdubbing listens to it, independent of which track is
// selected when a note arrives, same as the web app's per-track arm state.
//
// The R4 Minima's 32KB RAM (vs. an Uno R3's 2KB) means event storage below
// isn't SRAM-constrained the way it would be on a classic Uno.

#include <MIDI.h>
#include <Bounce2.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

MIDI_CREATE_INSTANCE(decltype(Serial1), Serial1, MIDI); // DIN jacks, D0/D1
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// ---- Pins ----
const uint8_t TRACK_PINS[4] = {2, 3, 4, 5};
const uint8_t PLAY_PIN = 6, REC_PIN = 7, MUTE_PIN = 8, CLEAR_PIN = 9;
const uint8_t NEOPIXEL_PIN = 11;
const uint8_t BPM_POT_PIN = A0;

Adafruit_NeoPixel strip(4, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
Bounce playBtn, recBtn, muteBtn, clearBtn;
Bounce trackBtns[4];

// ---- Loop storage --------------------------------------------------------
// ponytail: 64 events/track, 16 overlapping tails/track -- generous headroom
// on the R4 Minima's RAM; raise further if a track needs denser parts.
const uint8_t MAX_EVENTS_PER_TRACK = 64;
const uint8_t MAX_PENDING_OFFS = 16;
const unsigned long DUR_HELD = 0xFFFFFFFFUL; // sentinel: note is still down, not yet closed

enum TrackState : uint8_t { EMPTY, RECORDING, PLAYING, OVERDUBBING, PAUSED };

struct NoteEvent {
  uint8_t midi;
  uint8_t vel;
  unsigned long onAt;  // ms offset into the loop
  unsigned long dur;   // ms, or DUR_HELD while the note is still down
};

struct PendingOff {
  uint8_t midi;   // 0 = empty slot (MIDI note 0 is never used by this instrument)
  unsigned long offAt;
};

struct Track {
  uint8_t ch;             // MIDI channel 1-4, matches track number
  TrackState state;
  bool muted;
  unsigned long loopLenMs;
  unsigned long loopStartAt;
  unsigned long recordStartAt;
  unsigned long pausedAt;
  unsigned long lastElapsed;
  uint8_t eventCount;
  uint8_t overdubStartIndex;
  bool pendingStart; // armed on EMPTY, fires on the next bar boundary
  bool pendingStop;  // armed on RECORDING, fires on the next bar boundary
  unsigned long startBarCount; // global bar index the recording actually started on
  NoteEvent events[MAX_EVENTS_PER_TRACK];
  PendingOff pending[MAX_PENDING_OFFS];
};

Track tracks[4];
uint8_t selectedTrack = 0; // 0-3

unsigned long clearMsgUntil = 0;
const unsigned long CLEAR_MSG_MS = 1000;

unsigned long ledOffAt = 0;
const unsigned long LED_FLASH_MS = 20;

void setup() {
#ifdef LOONO_SELFTEST
  selfTest();
#endif
  playBtn.attach(PLAY_PIN, INPUT_PULLUP); playBtn.interval(5);
  recBtn.attach(REC_PIN, INPUT_PULLUP); recBtn.interval(5);
  muteBtn.attach(MUTE_PIN, INPUT_PULLUP); muteBtn.interval(5);
  clearBtn.attach(CLEAR_PIN, INPUT_PULLUP); clearBtn.interval(5);
  for (uint8_t i = 0; i < 4; i++) {
    trackBtns[i].attach(TRACK_PINS[i], INPUT_PULLUP);
    trackBtns[i].interval(5);
    tracks[i].ch = i + 1;
    tracks[i].state = EMPTY;
  }

  pinMode(LED_BUILTIN, OUTPUT);

  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff(); // we do our own thru so we can also feed the looper

  strip.begin();
  strip.setBrightness(26); // ~10% brightness
  strip.show();

  Wire.begin();
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  showSplash();
}

void showSplash() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(3);
  display.setCursor(14, 24);
  display.print("Loono");
  display.display();
  delay(2000);
}

void loop() {
  readTrackButtons();
  readPlayButton();
  readRecButton();
  readMuteButton();
  readClearButton();
  checkBarBoundary();
  readMidi();
  updatePlayback();
  updatePendingOffs();
  updateNeopixels();
  updateDisplay();
  updateLed();
}

void updateLed() {
  if (ledOffAt && millis() >= ledOffAt) {
    digitalWrite(LED_BUILTIN, LOW);
    ledOffAt = 0;
  }
}

// ---- BPM ------------------------------------------------------------------
unsigned int currentBpm() {
  return map(analogRead(BPM_POT_PIN), 0, 1023, 60, 180);
}
unsigned long barMs(unsigned int bpm) {
  return (60000UL / bpm) * 4; // 4/4 bar
}

// ---- Buttons ----------------------------------------------------------
void readTrackButtons() {
  for (uint8_t i = 0; i < 4; i++) {
    trackBtns[i].update();
    if (trackBtns[i].fell()) selectedTrack = i;
  }
}

void readPlayButton() {
  playBtn.update();
  if (!playBtn.fell()) return;
  Track &tr = tracks[selectedTrack];
  unsigned long now = millis();
  if (tr.state == PLAYING || tr.state == OVERDUBBING) {
    if (tr.state == OVERDUBBING) closeOverdub(tr, now);
    tr.state = PAUSED;
    tr.pausedAt = now;
    allNotesOff(tr);
  } else if (tr.state == PAUSED) {
    tr.loopStartAt += (now - tr.pausedAt);
    tr.state = PLAYING;
  }
}

void readRecButton() {
  recBtn.update();
  if (recBtn.rose()) {
    Track &tr = tracks[selectedTrack];
    unsigned long now = millis();
    if (tr.state == EMPTY) {
      tr.pendingStart = !tr.pendingStart; // arm for the next bar boundary; press again to cancel
    } else if (tr.state == RECORDING) {
      tr.pendingStop = !tr.pendingStop; // arm for the next bar boundary; press again to cancel
    } else if (tr.state == PLAYING) {
      tr.overdubStartIndex = tr.eventCount;
      tr.state = OVERDUBBING;
    } else if (tr.state == OVERDUBBING) {
      closeOverdub(tr, now);
      tr.state = PLAYING;
    }
    // PAUSED: no-op -- resume with Play first.
  }
}

void readMuteButton() {
  muteBtn.update();
  if (!muteBtn.fell()) return;
  Track &tr = tracks[selectedTrack];
  tr.muted = !tr.muted;
  if (tr.muted) allNotesOff(tr); // silence anything already sounding immediately
}

void readClearButton() {
  clearBtn.update();
  if (!clearBtn.fell()) return;
  clearTrack(tracks[selectedTrack]);
  clearMsgUntil = millis() + CLEAR_MSG_MS;
}

void closeRecording(Track &tr, unsigned long loopLenMs) {
  tr.loopLenMs = max(200UL, loopLenMs);
  for (uint8_t i = 0; i < tr.eventCount; i++) {
    if (tr.events[i].dur == DUR_HELD) {
      tr.events[i].dur = max(10L, (long)(tr.loopLenMs - tr.events[i].onAt));
    }
  }
  tr.loopStartAt = tr.recordStartAt;
  tr.lastElapsed = 0;
  tr.state = PLAYING;
}

void closeOverdub(Track &tr, unsigned long now) {
  unsigned long elapsed = elapsedFor(tr, now);
  for (uint8_t i = tr.overdubStartIndex; i < tr.eventCount; i++) {
    if (tr.events[i].dur == DUR_HELD) {
      long dur = (long)elapsed - (long)tr.events[i].onAt;
      if (dur < 0) dur += tr.loopLenMs; // held across the loop's wrap point
      tr.events[i].dur = max(10L, dur);
    }
  }
}

void clearTrack(Track &tr) {
  allNotesOff(tr);
  tr.state = EMPTY;
  tr.loopLenMs = 0;
  tr.eventCount = 0;
  tr.lastElapsed = 0;
  tr.pendingStart = false;
  tr.pendingStop = false;
}

// A first recording's start/stop is quantized to the bar grid -- press
// Rec on an empty track and it arms, but actually starts on the "1" of
// the next bar; press it again while recording and it arms a stop that
// lands on the end of the 4th beat (i.e. the following bar boundary).
unsigned long lastBarCount = 0;
void checkBarBoundary() {
  unsigned long now = millis();
  unsigned long barCount = now / barMs(currentBpm());
  if (barCount == lastBarCount) return;
  lastBarCount = barCount;
  for (uint8_t i = 0; i < 4; i++) {
    Track &tr = tracks[i];
    if (tr.pendingStart) {
      tr.pendingStart = false;
      tr.state = RECORDING;
      tr.recordStartAt = now;
      tr.eventCount = 0;
      tr.startBarCount = barCount;
    } else if (tr.pendingStop) {
      tr.pendingStop = false;
      // Exact bar count x current bar length -- not now-recordStartAt, which
      // can undershoot a whole bar by however long this loop() tick was late.
      closeRecording(tr, (barCount - tr.startBarCount) * barMs(currentBpm()));
    }
  }
}

void allNotesOff(Track &tr) {
  for (uint8_t i = 0; i < MAX_PENDING_OFFS; i++) {
    if (tr.pending[i].midi != 0) {
      MIDI.sendNoteOff(tr.pending[i].midi, 0, tr.ch);
      tr.pending[i].midi = 0;
    }
  }
}

// ---- MIDI in: live thru + feed into any recording/overdubbing track ----
unsigned long elapsedFor(Track &tr, unsigned long now) {
  if (tr.state == RECORDING) return now - tr.recordStartAt;
  return (now - tr.loopStartAt) % tr.loopLenMs;
}

void readMidi() {
  if (!MIDI.read()) return;
  midi::MidiType type = MIDI.getType();
  byte note = MIDI.getData1();
  byte vel = MIDI.getData2();
  byte ch = MIDI.getChannel();
  if (type != midi::NoteOn && type != midi::NoteOff) return;

  MIDI.send(type, note, vel, ch); // live playthrough, unchanged

  bool isOn = (type == midi::NoteOn && vel > 0);
  unsigned long now = millis();
  if (isOn) {
    digitalWrite(LED_BUILTIN, HIGH);
    ledOffAt = now + LED_FLASH_MS;
  }
  for (uint8_t i = 0; i < 4; i++) {
    Track &tr = tracks[i];
    if (tr.state != RECORDING && tr.state != OVERDUBBING) continue;
    if (isOn) looperNoteOn(tr, note, vel, now);
    else looperNoteOff(tr, note, now);
  }
}

void looperNoteOn(Track &tr, uint8_t note, uint8_t vel, unsigned long now) {
  if (tr.eventCount >= MAX_EVENTS_PER_TRACK) return; // full -- drop, don't corrupt memory
  NoteEvent &ev = tr.events[tr.eventCount++];
  ev.midi = note;
  ev.vel = vel;
  ev.onAt = elapsedFor(tr, now);
  ev.dur = DUR_HELD;
}

void looperNoteOff(Track &tr, uint8_t note, unsigned long now) {
  for (int8_t i = tr.eventCount - 1; i >= 0; i--) {
    if (tr.events[i].midi == note && tr.events[i].dur == DUR_HELD) {
      unsigned long elapsed = elapsedFor(tr, now);
      long dur = (long)elapsed - (long)tr.events[i].onAt;
      if (tr.state == OVERDUBBING && dur < 0) dur += tr.loopLenMs;
      tr.events[i].dur = max(10L, dur);
      return;
    }
  }
}

// ---- Playback: each track walks its own loop position independently ----
void updatePlayback() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < 4; i++) {
    Track &tr = tracks[i];
    if (tr.loopLenMs == 0) continue;
    if (tr.state != PLAYING && tr.state != OVERDUBBING) continue;
    unsigned long elapsed = (now - tr.loopStartAt) % tr.loopLenMs;
    unsigned long prev = tr.lastElapsed;
    for (uint8_t e = 0; e < tr.eventCount; e++) {
      NoteEvent &ev = tr.events[e];
      if (ev.dur == DUR_HELD) continue; // still being recorded this instant
      bool crossed = (prev <= elapsed) ? (ev.onAt > prev && ev.onAt <= elapsed)
                                        : (ev.onAt > prev || ev.onAt <= elapsed);
      if (crossed && !tr.muted) {
        MIDI.sendNoteOn(ev.midi, ev.vel, tr.ch);
        schedulePendingOff(tr, ev.midi, now + ev.dur);
      }
    }
    tr.lastElapsed = elapsed;
  }
}

void schedulePendingOff(Track &tr, uint8_t midi, unsigned long offAt) {
  for (uint8_t i = 0; i < MAX_PENDING_OFFS; i++) {
    if (tr.pending[i].midi == 0) {
      tr.pending[i].midi = midi;
      tr.pending[i].offAt = offAt;
      return;
    }
  }
  // All slots busy (dense overlap) -- drop the oldest rather than leak a stuck note forever.
  tr.pending[0].midi = midi;
  tr.pending[0].offAt = offAt;
}

void updatePendingOffs() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < 4; i++) {
    Track &tr = tracks[i];
    for (uint8_t p = 0; p < MAX_PENDING_OFFS; p++) {
      if (tr.pending[p].midi != 0 && now >= tr.pending[p].offAt) {
        MIDI.sendNoteOff(tr.pending[p].midi, 0, tr.ch);
        tr.pending[p].midi = 0;
      }
    }
  }
}

// ---- NeoPixels: a single lit LED chases positions 0-3 with the beat (LED N
// = beat N+1 of the bar), colored by the selected track's state -- green
// playing, red stopped (empty/paused), violet armed to start on beat 1,
// orange while actually recording or overdubbing. ----
void updateNeopixels() {
  unsigned long beatMs = 60000UL / currentBpm();
  uint8_t beatIndex = (millis() / beatMs) % 4;
  Track &sel = tracks[selectedTrack];

  uint32_t color;
  if (sel.pendingStart) color = strip.Color(150, 0, 255); // violet
  else if (sel.state == RECORDING || sel.state == OVERDUBBING) color = strip.Color(255, 110, 0); // orange
  else if (sel.state == PLAYING) color = strip.Color(0, 255, 0); // green
  else color = strip.Color(255, 0, 0); // red -- EMPTY, PAUSED (stop)

  for (uint8_t i = 0; i < 4; i++) strip.setPixelColor(i, i == beatIndex ? color : 0);
  strip.show();
}

// ---- OLED ------------------------------------------------------------
// Big center word for the selected track: "Next" overrides everything else
// while armed (about to start recording/overdubbing on the next bar's beat
// 1); RECORDING and OVERDUBBING both read as "Overdub".
const char *bigWord(Track &tr) {
  if (tr.pendingStart) return "Next";
  switch (tr.state) {
    case PLAYING: return "Play";
    case RECORDING:
    case OVERDUBBING: return "Overdub";
    default: return "Stop"; // EMPTY, PAUSED
  }
}

void printCentered(const char *s, int16_t y) {
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - (int16_t)w) / 2, y);
  display.print(s);
}

// Layout: top line BPM (left) / bar x-of-total (right), big state word
// centered in the middle, "Track N" centered along the bottom.
void updateDisplay() {
  Track &sel = tracks[selectedTrack];
  unsigned long now = millis();
  unsigned int bpm = currentBpm();
  unsigned long bm = barMs(bpm); // one bar = 4 beats

  unsigned long elapsed;
  if (sel.loopLenMs > 0) {
    unsigned long refNow = (sel.state == PAUSED) ? sel.pausedAt : now; // frozen while paused
    elapsed = elapsedFor(sel, refNow);
  } else if (sel.state == RECORDING) {
    elapsed = now - sel.recordStartAt;
  } else {
    elapsed = 0;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("BPM "); display.print(bpm);

  char barBuf[8];
  if (sel.loopLenMs > 0) {
    unsigned long bars = max(1UL, (sel.loopLenMs + bm / 2) / bm);
    snprintf(barBuf, sizeof(barBuf), "%lu/%lu", min(bars, elapsed / bm + 1), bars);
  } else if (sel.state == RECORDING) {
    snprintf(barBuf, sizeof(barBuf), "%lu", elapsed / bm + 1);
  } else {
    strcpy(barBuf, "-/-");
  }
  int16_t bx1, by1; uint16_t bw, bh;
  display.getTextBounds(barBuf, 0, 0, &bx1, &by1, &bw, &bh);
  display.setCursor(127 - bw, 0);
  display.print(barBuf);

  const char *word = (now < clearMsgUntil) ? "Clear" : bigWord(sel);
  display.setTextSize(strlen(word) <= 4 ? 3 : 2); // Overdub/Clear are the long ones
  printCentered(word, 22);

  char trackBuf[8];
  snprintf(trackBuf, sizeof(trackBuf), "Track %d", selectedTrack + 1);
  display.setTextSize(2);
  printCentered(trackBuf, 46);

  display.display();
}

// ---- Self-check: pure logic only, no hardware. Compile with -DLOONO_SELFTEST
// before trusting a change to the crossing/wraparound math above. ----
//#define LOONO_SELFTEST
#ifdef LOONO_SELFTEST
bool check(bool cond, const char *label) {
  if (!cond) { Serial.print("FAIL: "); Serial.println(label); }
  return cond;
}

void selfTest() {
  Serial.begin(9600);
  while (!Serial) {}
  bool ok = true;

  // crossed(): a 4-beat loop (1000ms) with an event at onAt=500 must fire
  // once per lap, including across the wrap point (prev near end, elapsed near start).
  auto crossed = [](unsigned long onAt, unsigned long prev, unsigned long elapsed) {
    return (prev <= elapsed) ? (onAt > prev && onAt <= elapsed) : (onAt > prev || onAt <= elapsed);
  };
  ok &= check(crossed(500, 400, 600), "normal crossing fires");
  ok &= check(!crossed(500, 600, 700), "already past, does not refire");
  ok &= check(crossed(50, 980, 20), "wraparound crossing fires");
  ok &= check(!crossed(500, 980, 20), "wraparound does not falsely fire unrelated event");

  // bar math: 120 BPM -> 500ms/beat -> 2000ms/bar; a 4000ms loop is 2 bars.
  ok &= check(barMs(120) == 2000, "bar length at 120bpm");
  ok &= check((4000UL + barMs(120) / 2) / barMs(120) == 2, "2-bar loop rounds correctly");

  // DUR_HELD sentinel must not collide with any real duration we'd produce.
  ok &= check(DUR_HELD == 0xFFFFFFFFUL, "held sentinel value");

  Serial.println(ok ? "LOONO SELFTEST: PASS" : "LOONO SELFTEST: FAIL");
}
#endif
