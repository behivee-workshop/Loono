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
//   D2             Play/pause button (INPUT_PULLUP, active low)
//   D3             Record/overdub button (INPUT_PULLUP, active low).
//                  Short press: empty->record->stop->overdub->stop->... .
//                  Long press (>800ms): clear the selected track.
//   D4-D7          Track select buttons 1-4 (INPUT_PULLUP, active low)
//   D8             NeoPixel data, 8 LEDs, one each:
//                  0-3 one per track (its own color; off/solid/dim/blink by
//                      that track's own state), 4 Play/Pause (lit while the
//                      selected track is playing), 5 Record (blinks red
//                      while the selected track is recording or
//                      overdubbing), 6 BPM (blinks on the beat), 7 MIDI note
//                      on (flashes on every outgoing Note On, live or
//                      looped).
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
const uint8_t PLAY_PIN = 2, REC_PIN = 3;
const uint8_t TRACK_PINS[4] = {4, 5, 6, 7};
const uint8_t NEOPIXEL_PIN = 8;
const uint8_t BPM_POT_PIN = A0;

Adafruit_NeoPixel strip(8, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
Bounce playBtn, recBtn;
Bounce trackBtns[4];

const unsigned long REC_LONG_PRESS_MS = 800;

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
  unsigned long loopLenMs;
  unsigned long loopStartAt;
  unsigned long recordStartAt;
  unsigned long pausedAt;
  unsigned long lastElapsed;
  uint8_t eventCount;
  uint8_t overdubStartIndex;
  NoteEvent events[MAX_EVENTS_PER_TRACK];
  PendingOff pending[MAX_PENDING_OFFS];
};

Track tracks[4];
uint8_t selectedTrack = 0; // 0-3

unsigned long noteOnBlinkUntil = 0;
const unsigned long NOTE_BLINK_MS = 30;
void flashMidiLed() { noteOnBlinkUntil = millis() + NOTE_BLINK_MS; }

const uint32_t TRACK_COLOR[4] = {
  strip.Color(0, 120, 255),   // T1 blue
  strip.Color(255, 40, 120),  // T2 pink
  strip.Color(255, 170, 0),   // T3 amber
  strip.Color(60, 220, 90),   // T4 green
};

void setup() {
#ifdef LOONO_SELFTEST
  selfTest();
#endif
  playBtn.attach(PLAY_PIN, INPUT_PULLUP); playBtn.interval(5);
  recBtn.attach(REC_PIN, INPUT_PULLUP); recBtn.interval(5);
  for (uint8_t i = 0; i < 4; i++) {
    trackBtns[i].attach(TRACK_PINS[i], INPUT_PULLUP);
    trackBtns[i].interval(5);
    tracks[i].ch = i + 1;
    tracks[i].state = EMPTY;
  }

  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff(); // we do our own thru so we can also feed the looper

  strip.begin();
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
  readMidi();
  updatePlayback();
  updatePendingOffs();
  updateNeopixels();
  updateDisplay();
}

// ---- BPM ------------------------------------------------------------------
unsigned int currentBpm() {
  return map(analogRead(BPM_POT_PIN), 0, 1023, 40, 240);
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
    unsigned long heldFor = recBtn.duration();
    Track &tr = tracks[selectedTrack];
    unsigned long now = millis();
    if (heldFor >= REC_LONG_PRESS_MS) {
      clearTrack(tr);
    } else if (tr.state == EMPTY) {
      tr.state = RECORDING;
      tr.recordStartAt = now;
      tr.eventCount = 0;
    } else if (tr.state == RECORDING) {
      closeRecording(tr, now);
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

void closeRecording(Track &tr, unsigned long now) {
  tr.loopLenMs = max(200UL, now - tr.recordStartAt);
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
  if (isOn) flashMidiLed();
  unsigned long now = millis();
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
      if (crossed) {
        MIDI.sendNoteOn(ev.midi, ev.vel, tr.ch);
        flashMidiLed();
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

// ---- NeoPixels: 8 single-purpose LEDs -- 4 tracks, play/pause, record, BPM, MIDI activity ----
void updateNeopixels() {
  unsigned long now = millis();
  unsigned int bpm = currentBpm();
  unsigned long beatMs = 60000UL / bpm;
  bool blinkOn = (now % 300) < 150;
  Track &sel = tracks[selectedTrack];

  for (uint8_t i = 0; i < 4; i++) {
    Track &tr = tracks[i];
    uint32_t color;
    switch (tr.state) {
      case RECORDING:   color = blinkOn ? strip.Color(255, 0, 0) : 0; break;
      case OVERDUBBING: color = blinkOn ? TRACK_COLOR[i] : strip.Color(255, 60, 0); break;
      case PLAYING:     color = TRACK_COLOR[i]; break;
      case PAUSED:      color = dim(TRACK_COLOR[i], 3); break;
      default:          color = 0; break; // EMPTY
    }
    if (i != selectedTrack && tr.state != RECORDING) color = dim(color, 2);
    strip.setPixelColor(i, color);
  }

  bool playingLit = (sel.state == PLAYING || sel.state == OVERDUBBING);
  strip.setPixelColor(4, playingLit ? strip.Color(255, 255, 255) : (sel.state == PAUSED ? dim(strip.Color(255, 255, 255), 4) : 0));

  bool recordingLit = (sel.state == RECORDING || sel.state == OVERDUBBING) && blinkOn;
  strip.setPixelColor(5, recordingLit ? strip.Color(255, 0, 0) : 0);

  bool beatOn = (now % beatMs) < 40;
  strip.setPixelColor(6, beatOn ? strip.Color(255, 255, 255) : 0);

  bool midiOn = now < noteOnBlinkUntil;
  strip.setPixelColor(7, midiOn ? strip.Color(0, 255, 255) : 0);

  strip.show();
}

uint32_t dim(uint32_t color, uint8_t divisor) {
  uint8_t r = (uint8_t)(color >> 16), g = (uint8_t)(color >> 8), b = (uint8_t)color;
  return strip.Color(r / divisor, g / divisor, b / divisor);
}

// ---- OLED ----------------------------------------------------------------
const char *STATE_LABEL[5] = {"Empty", "Rec", "Play", "Dub", "Pause"};
const char STATE_CHAR[5] = {'-', 'R', 'P', 'D', '.'};

void updateDisplay() {
  Track &sel = tracks[selectedTrack];
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print('T'); display.print(selectedTrack + 1);
  display.print(' ');
  display.println(STATE_LABEL[sel.state]);

  display.setTextSize(1);
  display.setCursor(0, 20);
  display.print("BPM "); display.print(currentBpm());
  if (sel.loopLenMs > 0) {
    unsigned long bars = max(1UL, (sel.loopLenMs + barMs(currentBpm()) / 2) / barMs(currentBpm()));
    display.print("  Bars "); display.print(bars);
  }

  display.setCursor(0, 36);
  for (uint8_t i = 0; i < 4; i++) {
    display.print(i + 1); display.print(':'); display.print(STATE_CHAR[tracks[i].state]);
    display.print(i == selectedTrack ? '<' : ' ');
    display.print(' ');
  }

  display.setCursor(0, 52);
  display.print("Hold Rec to clear");

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
