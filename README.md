# Loono

A 4-track MIDI looper for the Arduino Uno R4 Minima — the pedal-sized hardware twin of `app/loop`, the Keybench Loop web app. One button records and overdubs, one plays and pauses, four pick a track.

| | |
|---|---|
| **Firmware** | `Loono.ino` |
| **Board** | Uno R4 Minima |
| **Tracks** | 4 · MIDI channels 1–4 |
| **Flash** | 62,212 / 262,144 B (23%) |
| **RAM** | 9,628 / 32,768 B (29%) |

![Loono faceplate diagram showing MIDI IN and OUT jacks, the OLED screen, the 8-pixel NeoPixel strip, the BPM pot, the play/pause and record buttons, and four track-select buttons, each labeled with its Arduino pin.](docs/images/panel.svg)

*Fig. 1 — Faceplate layout. Track button rings match the exact NeoPixel RGB values each track lights up with (see [Visual feedback](#visual-feedback)), so the panel and the firmware agree on color before you've wired a single LED.*

## Overview

Loono ports the state machine behind the Loop web app — `empty → recording → playing ↔ overdubbing ↔ paused`, independently per track — onto six buttons, a pot, and a strip of light. Four tracks, four MIDI channels, no quantization: punching in and out happens the instant you press the button, exactly like the web app running with no metronome bar in flight.

Only one button each is spent on Play/Pause and Record; everything they do applies to whichever track is currently selected. MIDI input is different — it is always echoed straight through, and it is heard by *every* track that happens to be armed, not just the selected one.

## Pin map

| Pin | Role | Notes |
|---|---|---|
| **D0 / D1** | Serial1 · MIDI in/out | 31,250 baud via the usual optocoupler-in / resistor-out circuit. Separate from the native-USB `Serial`, so debug prints and MIDI run at the same time. |
| **D2** | Play / Pause | INPUT_PULLUP, active low. Acts on the selected track only. |
| **D3** | Record / Overdub | Short press cycles record → overdub. Hold ≥800 ms to clear the selected track. |
| **D4 – D7** | Track select 1–4 | Changes which track Play and Record act on. |
| **D8** | NeoPixel data | 8 LEDs, one per role — see [Visual feedback](#visual-feedback). |
| **A0** | BPM pot | Maps to 40–240 BPM. Drives the bar-count readout and the idle metronome pulse only — never quantizes a punch in/out. |
| **A4 / A5** | OLED I2C | SSD1306 128×64 @ 0x3C. |

## Controls

What each button does depends entirely on the selected track's current state — the table is the firmware's `readPlayButton()` / `readRecButton()` logic read straight off.

| Track state | Play/Pause (D2) | Record, short press (D3) |
|---|---|---|
| Empty | — | Start recording |
| Recording | — | Close loop → Playing |
| Playing | Pause | Start overdub |
| Overdubbing | Close overdub, then pause | Close overdub → Playing |
| Paused | Resume | — (resume with Play first) |

A record-button hold of 800 ms or more clears the selected track from any state — stops its sounding notes, drops its events, and returns it to Empty. There's no separate clear button, so this is the only way back to Empty short of a reboot.

## Signal flow

![Diagram showing MIDI IN splitting into two paths: an immediate thru echo straight to MIDI OUT, and a capture path through armed tracks and an event buffer into a playback scanner that re-sends notes on each track's own MIDI channel, which also feeds back into MIDI OUT.](docs/images/signal-flow.svg)

*Fig. 2 — Two paths share one input. Every note goes straight through to MIDI OUT unchanged (top); any track that's armed also captures it into its own loop, which plays back later on that track's channel (bottom, highlighted) — the same note can leave on two different channels at two different times.*

## BPM & bar math

The A0 pot maps linearly to **40–240 BPM**. It never touches when a recording starts or stops — that's always immediate, on the button press. What it does drive:

| Used for | Formula |
|---|---|
| Bar length | `barMs = (60000 / bpm) × 4` — one 4/4 bar |
| Bar-count readout | `bars = round(loopLenMs / barMs)`, shown on the OLED |
| Idle metronome pulse | BPM pixel flashes every `60000 / bpm` ms |

## Visual feedback

One NeoPixel per job, not per track-and-state combination: four pixels are the tracks, four are single-purpose indicators shared by the whole instrument.

| Pixel | Role | Color | Reads |
|---|---|---|---|
| 0 | Track 1 | `rgb(0,120,255)` | |
| 1 | Track 2 | `rgb(255,40,120)` | |
| 2 | Track 3 | `rgb(255,170,0)` | |
| 3 | Track 4 | `rgb(60,220,90)` | |
| 0–3 | Track status | — | Off (Empty) · blinks red (Recording) · blinks track color/orange (Overdubbing) · solid track color (Playing) · track color dimmed ÷3 (Paused). Non-selected tracks also dim ÷2, except mid-recording — a take in progress always reads full brightness. |
| 4 | Play / Pause | white | Lit while the *selected* track is playing or overdubbing, dim while paused, off if there's nothing to play yet. |
| 5 | Record | red | Blinks while the selected track is recording or overdubbing. Off otherwise — a direct read of "is it recording or not." |
| 6 | BPM | white | Flashes on every beat at the pot's current tempo. Runs independently of any track — the one pixel that's always a metronome. |
| 7 | MIDI note on | cyan | Flashes for 30 ms on every outgoing Note On — live playthrough and looped playback both count. |

## Memory headroom

Loono targets the R4 Minima specifically because the looper's event storage doesn't fit comfortably in an Uno R3's 2 KB of SRAM once the OLED's 1,024-byte framebuffer is accounted for.

| | Uno R3 | Uno R4 Minima (shipped) |
|---|---|---|
| SRAM | 2 KB | 32 KB |
| Events / track | 16 (forced) | 64 |
| Overlapping tails / track | 4 (forced) | 16 |
| Event timestamp | 16-bit — 65.5s loop cap | 32-bit — effectively unbounded |
| MIDI UART | Shares USB `Serial` (D0/D1) | Independent `Serial1` (D0/D1) |

Measured build on the shipped target: `flash 62,212 / 262,144 B (23%)`, `RAM 9,628 / 32,768 B (29%)`.

## Build & verify

- [ ] Libraries: MIDI Library 5.0.2, Bounce2 2.71, Adafruit GFX 1.12.6 + BusIO, Adafruit SSD1306 2.5.17, Adafruit NeoPixel 1.15.5
- [ ] Board: `arduino:renesas_uno:minima`
- [ ] Logic self-check: uncomment `#define LOONO_SELFTEST` and watch the native `Serial` @9600 for `PASS`/`FAIL` on the crossing, wraparound, and bar-math logic before trusting a change to it
- [ ] No stuck notes on pause or clear — both flush every pending note-off immediately

---

Port of `app/loop` · Keybench / UnoWeb
