# Loono — board & sequencer spec

Planning reference for the pin map, panel controls, and sequencer behavior. This is the agreed basis for the firmware; update it here first if any of it changes.

## Signal path

Keylab notes enter on the opto-isolated MIDI in. Loono rewrites only note on/off to the active channel; everything else (CC, pitch bend, clock) passes through untouched and continues down the DIN chain to the synths.

```mermaid
flowchart LR
    KB[Keylab] -->|DIN out| MI[MIDI in · RX]
    MI --> LOONO[["LOONO<br/>note channel remap"]]
    LOONO -->|MIDI out · TX| SA[Synth A]
    SA -->|thru| SB[Synth B …]
    CTRL[D2–D9 buttons + A0 pot] -.-> LOONO
    LOONO -.-> OUT[OLED + NeoPixel ×4]
```

## Pin map

| Pin | Function | Notes |
|---|---|---|
| `D2`–`D5` | Channel buttons 1–4 | Select active channel; second press on the active one toggles it between standard and sequencer mode. Repurposed while edit is armed — see [Edit navigation](#edit-navigation-d2d5-while-armed) |
| `D6` | Play / Pause | Starts or stops the shared clock for all 4 lanes |
| `D7` | Edit | Arms step recording on the active channel |
| `D8` | Skip | Edit mode only — writes a rest, advances |
| `D9` | Tie | Edit mode only — sustains previous step, advances |
| `A0` | Tempo pot | 60–160 BPM, live while playing |
| `RX` / `D0` | MIDI in | Opto-isolated DIN in from the Keylab, 31250 baud |
| `TX` / `D1` | MIDI out | DIN out, daisy-chains to the synths |
| `SDA` / `SCL` | OLED | 128×64 SSD1306, I2C |
| `D11` | NeoPixel ×4 | One LED per channel, mirrors active/mode state |

## Channel modes

Each channel is independently in one of two modes — this is per channel, not tied to which one is currently active:

- **Standard** — no lane. Notes played on the Keylab while this channel is active pass straight through on its MIDI channel, as today.
- **Sequencer** — the channel's 16-step lane exists and joins the shared clock. Its own play/stopped state follows the global transport (see Playback below), not a per-channel one.

Active channel selection and per-channel mode are independent: several channels can be in sequencer mode at once, and only one is "active" (the live-note / edit target) at a time. For example: channel 1's lane plays on its own while channel 2 stays in standard mode and you play live over it, or both channel 1 and channel 2 can be in sequencer mode and play their lanes together in sync.

| State | LED |
|---|---|
| Active | Solid green |
| Inactive, standard | Off |
| Inactive, sequencer | *TBD — see open questions* |

## Sequencer model

Each of the 4 channels owns an independent 16-step lane. Every step is exactly one of three types:

- **chord** — one or more notes captured together, triggered at that step
- **rest** — nothing plays
- **tie** — no new note; whatever is sounding from an earlier step keeps ringing through it

Example lane:

| Step | 1 | 2 | 3 | 4 | 5 | 6–16 |
|---|---|---|---|---|---|---|
| Content | `C4+E4` | rest | `G4` | tie | tie | rest |

Step 3's `G4` sustains through the two tied steps that follow it before the lane goes quiet.

## Recording (edit mode)

Edit arms the active channel only; the other three keep playing or resting as already programmed.

1. Press **Edit** (`D7`) — this stops the shared clock if it was running, and arms the active channel's cursor at step 1, always, regardless of where playback last was. `D2`–`D5` stop selecting channels for as long as edit stays armed.
2. Hold one or more keys on the Keylab. Any note pressed while at least one key is still down joins the same chord.
3. Release the last held key — the chord commits to the current step and the cursor advances one step (wraps 16→1).
4. **Skip** (`D8`) writes a rest at the cursor and advances, without touching the keyboard.
5. **Tie** (`D9`) marks the cursor as sustain-previous and advances.

### Edit navigation (`D2`–`D5` while armed)

| Button | Action |
|---|---|
| `D2` | Step back — moves the cursor to the previous step, no write |
| `D3` | Step forward — moves the cursor to the next step, no write |
| `D4` | Clear — sets the step at the cursor to rest; cursor does not move |
| `D5` | Exit edit — disarms edit mode; `D2`–`D5` revert to channel select |

`D7` (Edit) still arms and disarms edit mode on its own, same as before — `D5` is just the exit reachable from inside edit, since `D2`–`D5` no longer select channels while armed.

## Playback

One shared 16-step clock drives every channel in sequencer mode — step 1 fires for all of them together, then step 2, and so on, never independently. On each step, a sequencer-mode channel plays its chord, holds through any following ties, and goes silent on a rest. A channel left in standard mode takes no part in playback. **Play / Pause** starts and stops the shared clock for every sequencer-mode channel at once — there is no per-channel transport.

## Open questions

- **Chord size cap** — how many simultaneous notes should a step be allowed to hold, is there a polyphony limit worth enforcing on the record side?
- **Play restart vs. resume** — does pressing Play always restart the clock at step 1, or resume from wherever it was paused?
- **Velocity** — does a recorded chord keep the velocity it was played at, or get a fixed value?
- **Tie on step 1** — a tie with nothing before it has nothing to sustain, should it just behave like a rest?
- **Switching a channel to standard mid-playback** — does its sounding note cut immediately or ring out?
- **LED for an inactive sequencer-mode channel** — solid, dim, or blinking in time with the shared clock?
