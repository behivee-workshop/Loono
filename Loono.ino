// Loono — hardware scaffold
// Board: Arduino Uno R4 Minima
//
// Pins:
//   D2-D5   buttons bank A (INPUT_PULLUP)
//   D6-D9   buttons bank B (INPUT_PULLUP)
//   A0      potentiometer
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

const uint8_t BUTTON_PINS[] = {2, 3, 4, 5, 6, 7, 8, 9};
const uint8_t NUM_BUTTONS = sizeof(BUTTON_PINS) / sizeof(BUTTON_PINS[0]);
Bounce buttons[NUM_BUTTONS];

// Buttons D2-D5 (index 0-3) pick the MIDI out channel 1-4; LED N mirrors channel N.
uint8_t midiOutChannel = 1;

void setMidiOutChannel(uint8_t channel) {
  midiOutChannel = channel;
  pixels.clear();
  pixels.setPixelColor(channel - 1, pixels.Color(0, 20, 0));
  pixels.show();

  display.fillRect(0, 0, 40, 8, SSD1306_BLACK);
  display.setCursor(0, 0);
  display.print("ch ");
  display.print(channel);
  display.display();
}

void setup() {
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    buttons[i].attach(BUTTON_PINS[i], INPUT_PULLUP);
    buttons[i].interval(5);
  }

  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();

  pixels.begin();

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Loono");
  display.display();
  delay(2000);
  display.clearDisplay();
  display.display();

  setMidiOutChannel(midiOutChannel);
}

void loop() {
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    buttons[i].update();
    if (i < 4 && buttons[i].fell()) {
      setMidiOutChannel(i + 1);
    }
  }

  int potValue = analogRead(POT_PIN);

  while (MIDI.read()) {
    midi::MidiType type = MIDI.getType();
    if (type == midi::NoteOn || type == midi::NoteOff) {
      MIDI.send(type, MIDI.getData1(), MIDI.getData2(), midiOutChannel);
    } else {
      MIDI.send(type, MIDI.getData1(), MIDI.getData2(), MIDI.getChannel());
    }
  }
}
