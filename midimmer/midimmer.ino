#include <EEPROM.h>
#include <avr/wdt.h>
#include <avr/eeprom.h>
#include <MIDI.h>
#include <GyverTimers.h>
#include <SoftwareSerial.h>
#include <U8g2lib.h>
#include <qdec.h>
using namespace ::SimpleHacks;

#define NELEMS(x)  (sizeof(x) / sizeof((x)[0]))

const char * noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

const int OLED_DC_PIN = A3;
const int OLED_RES_PIN = A2;
const int OLED_MOSI_PIN = A1;
const int OLED_SCK_PIN = A0;
const int OLED_DEBOUNCE_MS = 100;
const int OLED_SLEEP_MS = 30000;

const int ROTARY_PIN_A = 12; // the first pin connected to the rotary encoder
const int ROTARY_PIN_B = 11; // the second pin connected to the rotary encoder
const int ROTARY_BUTTON_PIN = A5; // the button pin connected to the rotary encoder
const int BUTTONS_DEBOUNCE_MS[2] = {50, 3000}; // click, long click
const int DIM_AMOUNT = 2;
const int ZERO_PIN = 2;
const byte DIMMER_PINS[] = {4, 5};
const int MAX_NOTE_NUMBER = 127;
const float EXPONENTIAL_SCALE_R = 31.8975139158; // 255*log10(2)/log10(200)
const unsigned long MIDI_EFFECT_UNIT_SCALE = 5; // speed[0 .. 255] * MIDI_EFFECT_UNIT_SCALE = (ms)

byte dimmer[DIM_AMOUNT] = {0, 0};
byte dimmerCurrentCycle[DIM_AMOUNT] = {0, 0};
volatile int dimmerCounter = 0;

U8G2_SSD1306_128X64_NONAME_1_4W_SW_SPI u8g2(U8G2_R2, OLED_SCK_PIN, OLED_MOSI_PIN, A4, OLED_DC_PIN, OLED_RES_PIN);

QDecoder qdec(ROTARY_PIN_A, ROTARY_PIN_B, true);

unsigned long buttonDebounce = 0;
int buttonState = LOW;
int lastButtonState = LOW;
unsigned long currentMillis;
unsigned long oledLastUpdateMillis = 0;
unsigned long lastActionMillis = 0;

MIDI_CREATE_DEFAULT_INSTANCE();
byte lastMidiNoteNumber = 0;
unsigned long lastTriggeredMIDI[DIM_AMOUNT] = {0, 0};

typedef struct Encoder {
  byte value;
  byte minValue;  
  byte maxValue;
  byte step;
};

volatile Encoder *currentEncoder;

// -- ENCODER INTERRUPTS -------------------------------------------------------
 
void encoderHandle(void) {
  QDECODER_EVENT event = QDec<ROTARY_PIN_A, ROTARY_PIN_B, true>::update();
 
  if (event & QDECODER_EVENT_CW) {
    if (currentEncoder->value + currentEncoder->step <= currentEncoder->maxValue) 
      currentEncoder->value += currentEncoder->step;
      lastActionMillis = currentMillis;
  } else if (event & QDECODER_EVENT_CCW) {
    if (currentEncoder->value - currentEncoder->step >= currentEncoder->minValue) 
      currentEncoder->value -= currentEncoder->step;
      lastActionMillis = currentMillis;
  }
  return;
}

// -- MENU INTERFACE DECLARATION ---------------------------------------------------------------------------

typedef void (*DrawingFunc)();

void drawMenu(void);
void drawMidiDebugging(void);
void drawEditingModeScreen(void);
void drawModesEncoder(void);
void drawLevelEncoder(void);
void drawNoteEncoder(void);

byte lampModeHandle(byte lamp);
byte LFOModeHandle(byte lamp);
byte MIDI1ModeHandler(byte lamp);
byte MIDI2ModeHandler(byte lamp);

// -- MENU DECLARATION -----------------------------------------------------------------------------

typedef struct MenuItem {
  char *name;
  Encoder *encoders;
  DrawingFunc drawing;
};

typedef struct Menu {
  Encoder encoder;
  MenuItem items[];
};

typedef byte (*LampFunc)(byte lamp);
typedef struct Mode {
  char* name;
  byte menuDepth;
  LampFunc handle;
};

bool menuEditing = false;
bool midiDebugging = false;

byte selectedLamp = 0;
volatile Encoder nullEncoder = {0, 0, 0, 1};
const Mode modes[] = {
  {.name = "Lamp",  .menuDepth = 1, .handle = &lampModeHandler},
  {.name = "LFO",   .menuDepth = 3, .handle = &LFOModeHandler},
  {.name = "MIDI1", .menuDepth = 4, .handle = &MIDI1ModeHandler},
  {.name = "MIDI2", .menuDepth = 4, .handle = &MIDI2ModeHandler},
  {.name = "MIDIST", .menuDepth = 4, .handle = &MIDISTModeHandler},
};
volatile Encoder lampMode[DIM_AMOUNT]     = {{0, 0, NELEMS(modes)-1, 1}, {0, 0, 2, 1}};
volatile Encoder lampValueMax[DIM_AMOUNT] = {{ 255, 0, 255, 10 }, { 255, 0, 255, 10 }};
volatile Encoder lampValueMin[DIM_AMOUNT] = {{ 0, 0, 255, 10 }, { 0, 0, 255, 10 }};
volatile Encoder lampSpeed[DIM_AMOUNT]    = {{ 127, 0, 255, 10 }, { 127, 0, 255, 10 }};
volatile Encoder lampMIDINote[DIM_AMOUNT] = {{ 38, 0, MAX_NOTE_NUMBER, 1 }, { 50, 0, MAX_NOTE_NUMBER, 1 }};
Encoder* encoders[] = {
  &lampMode[0], &lampMode[1], 
  &lampValueMax[0], &lampValueMax[1], 
  &lampValueMin[0], &lampValueMin[1],
  &lampSpeed[0], &lampSpeed[1], 
  &lampMIDINote[0], &lampMIDINote[1],
};

Menu lampMenu = {
  .encoder = {1, 0, modes[0].menuDepth+2, 1}, // depth value and +-1 gap for menu paging
  .items = {
    {.name = "mode", .encoders = lampMode, .drawing = &drawModesEncoder}, 
    {.name = "max", .encoders = lampValueMax, .drawing = &drawLevelEncoder},
    {.name = "min", .encoders = lampValueMin, .drawing = &drawLevelEncoder},
    {.name = "speed", .encoders = lampSpeed, .drawing = &drawLevelEncoder},
    {.name = "note", .encoders = lampMIDINote,  .drawing = &drawNoteEncoder},
  }  
};
MenuItem *selectedMenuItem = &lampMenu.items[lampMenu.encoder.value-1];

// -- MENU Handler -------------------------------------------------------------------------------

void menuHandle(void) {
  bool buttonClicked[2] = {false, false};
  int readingButton = digitalRead(ROTARY_BUTTON_PIN)?LOW:HIGH;
  if (readingButton != buttonState) {  
    for (byte debounceInterval = 0; debounceInterval <= 1; ++debounceInterval) {
      if ((currentMillis - buttonDebounce) > BUTTONS_DEBOUNCE_MS[debounceInterval]) {
        buttonState = readingButton;
        if (readingButton == LOW) {
          lastActionMillis = currentMillis;
          buttonClicked[debounceInterval] = true;
        }
      }
    }
  }
  if (readingButton != lastButtonState) {
    buttonDebounce = currentMillis;
  }
  lastButtonState = readingButton;
  
  if (buttonClicked[1]) {
    if (midiDebugging) {
      currentEncoder = &lampMenu.encoder;
      menuEditing = false;
      midiDebugging = false;
    } else {
      currentEncoder = &nullEncoder;
      menuEditing = false;
      midiDebugging = true;
    }
  } else if (!midiDebugging && buttonClicked[0]) {
    if (menuEditing) {
      currentEncoder = &lampMenu.encoder;
      menuEditing = false;
    } else {
      currentEncoder = &selectedMenuItem->encoders[selectedLamp];
      menuEditing = true;
    }
  }
  if (lampMenu.encoder.value == 0) {
    if (selectedLamp == 0) {
      lampMenu.encoder.value = 1;
    } else {
      selectedLamp--;
      lampMenu.encoder.maxValue = modes[lampMode[selectedLamp].value].menuDepth+2;
      lampMenu.encoder.value = modes[lampMode[selectedLamp].value].menuDepth+1;
    }
  }
  if (lampMenu.encoder.value == lampMenu.encoder.maxValue) {
    if (selectedLamp == DIM_AMOUNT-1) {
      lampMenu.encoder.value--;
    } else {
      selectedLamp++;
      lampMenu.encoder.value = 1;
      lampMenu.encoder.maxValue = modes[lampMode[selectedLamp].value].menuDepth+2;
    }
  }
  selectedMenuItem = &lampMenu.items[lampMenu.encoder.value-1];
}

// -- MENU ITEMS VIEWS ---------------------------------------------------------------------------

void drawMenu(void) {
  byte itemsCount = modes[lampMode[0].value].menuDepth;
  for (byte i = 0; i <= itemsCount; ++i) 
  {
    u8g2.drawCircle( 8, 32+(2*i-itemsCount)*5, 
      (selectedLamp == 0 && i == lampMenu.encoder.value-1 ? 4 : 2));
  }
  itemsCount = modes[lampMode[1].value].menuDepth;
  for (byte i = 0; i <= itemsCount; ++i) 
  {
    u8g2.drawCircle( 120, 32+(2*i - itemsCount)*5, 
      (selectedLamp == 1 && i == lampMenu.encoder.value-1 ? 4 : 2));
  }
}


void drawChannel(void) {
  u8g2.setFont(u8g2_font_helvR12_tf);
  char channel[10] = "channel 1";
  channel[8] += selectedLamp;
  u8g2.drawStr(18, 15, channel);
}
 
void drawEditingModeScreen(void) {
  u8g2.drawLine(10, 18, 0, 41);
  u8g2.drawLine(0, 41, 10, 63);
  u8g2.drawLine(117, 18, 127, 41);
  u8g2.drawLine(127, 41, 117, 63);
}

void drawMidiDebugging(void) {
  u8g2.setFont(u8g2_font_helvR12_tf);
  u8g2.drawStr(30, 30, "MIDI Debug");
  char notestr[4] = "...";
  if (lastMidiNoteNumber != 0) {
    byte octave = int(lastMidiNoteNumber / 12);
    byte noteInOctave = lastMidiNoteNumber % 12;
    notestr[0] = 49+octave;
    notestr[1] = noteNames[noteInOctave][0];
    notestr[2] = noteNames[noteInOctave][1];
    notestr[3] = 0;
  }
  u8g2.setFont(u8g2_font_helvR24_tf);
  u8g2.drawStr(50, 60, notestr);

}

void drawModesEncoder(void) {    
  u8g2.setFont(u8g2_font_helvR24_tf);
  u8g2.drawStr(18, 60, modes[lampMode[selectedLamp].value].name);
}

void drawLevelEncoder(void) {    
  u8g2.setFont(u8g2_font_helvR12_tf);
  u8g2.drawStr(18,35,selectedMenuItem->name);
  byte value = selectedMenuItem->encoders[selectedLamp].value;
  byte x = 20 + int(87*value/255);
  byte y = 63 - int(43*value/255);
  u8g2.drawLine(20, 63, 107, 63);
  u8g2.drawLine(107, 63, 107, 20);
  u8g2.drawLine(107, 20, 20, 63);
  u8g2.drawTriangle(20, 63, x, 63, x, y);
}

void drawNoteEncoder(void) {    
  u8g2.setFont(u8g2_font_helvR12_tf);
  u8g2.drawStr(18,35,selectedMenuItem->name);
  byte noteNumber = selectedMenuItem->encoders[selectedLamp].value;
  byte octave = int(noteNumber / 12);
  byte noteInOctave = noteNumber % 12;
  u8g2.setFont(u8g2_font_helvR24_tf);
  char notestr[4] = {49+octave, noteNames[noteInOctave][0], noteNames[noteInOctave][1], 0};
  u8g2.drawStr(50,64,notestr);
}

// -- LAMPS Handler -------------------------------------------------------------------------------------

byte lampModeHandler(byte lamp) {
  return lampValueMax[lamp].value;
}

byte LFOModeHandler(byte lamp) {
  // I'd like to use sin, but it's heavy
  return lampValueMin[lamp].value + abs(255 - int(currentMillis / sqrt(lampSpeed[lamp].value+1))% 512)
    *(lampValueMax[lamp].value-lampValueMin[lamp].value)/255;
}

byte MIDI1ModeHandler(byte lamp) {
    unsigned long sinceLast = currentMillis - lastTriggeredMIDI[lamp];
    unsigned long interval = (lampSpeed[lamp].value+1) * MIDI_EFFECT_UNIT_SCALE;
    byte intervalMinMax = lampValueMax[lamp].value-lampValueMin[lamp].value;
    if (sinceLast >= interval) {
      return lampValueMin[lamp].value;
    } else if (sinceLast > interval*3/4) {
      return lampValueMin[lamp].value 
        + intervalMinMax - (intervalMinMax * (sinceLast*4 - interval*3)/interval);
    } else if (sinceLast <= interval/4) {
      return lampValueMin[lamp].value 
        + intervalMinMax * (sinceLast*4)/interval;
    }else{
     return lampValueMax[lamp].value;
    }
}

byte MIDI2ModeHandler(byte lamp) {
    unsigned long sinceLast = currentMillis - lastTriggeredMIDI[lamp];
    unsigned long interval = (lampSpeed[lamp].value+1) * MIDI_EFFECT_UNIT_SCALE;
    byte intervalMinMax = lampValueMax[lamp].value-lampValueMin[lamp].value;
    if (sinceLast >= interval) {
      return lampValueMin[lamp].value;
    } else if (sinceLast > interval*7/8) {
      return lampValueMin[lamp].value 
        + intervalMinMax - (intervalMinMax * (sinceLast*8 - interval*7)/interval);
    } else if (sinceLast <= interval/8) {
      return lampValueMin[lamp].value 
        + intervalMinMax * (sinceLast*8)/interval;
    }else{
      return lampValueMax[lamp].value;
    }
}

byte MIDISTModeHandler(byte lamp) {
  static unsigned long lastUpdatedMillis[DIM_AMOUNT] = {0};
  static byte value[DIM_AMOUNT] = {0};
  if (lastUpdatedMillis[lamp] == lastTriggeredMIDI[lamp])
    return value[lamp];
  lastUpdatedMillis[lamp] = lastTriggeredMIDI[lamp];
  value[lamp] += 63;
  if (value[lamp] > 250)
    value[lamp] = 0;
  return value[lamp];
}

// -- MIDI NOTE ON Handler ------------------------------------------------------------------------------
 
void handleNoteOn(byte channel, byte note, byte velocity) {
  lastMidiNoteNumber = note;
  for (byte i = 0; i < DIM_AMOUNT; i++) {
    if (midiDebugging)
      lastActionMillis = currentMillis;
    if (lampMIDINote[i].value == note 
    //  && (currentMillis - lastTriggeredMIDI[i]) > (lampSpeed[i].value+1) * MIDI_EFFECT_UNIT_SCALE
    )
      lastTriggeredMIDI[i] = currentMillis;
  }
}

// -- AC DIMMER ROUTINE ---------------------------------------------------------------------------------

void zeroCrossISR() {
  dimmerCounter = 255;
  for (byte i = 0; i < DIM_AMOUNT; i++) {
    dimmerCurrentCycle[i] = dimmer[i];
  }
  Timer2.restart();
}

ISR(TIMER2_A) {
  for (byte i = 0; i < DIM_AMOUNT; i++) {
    if (dimmerCounter == dimmerCurrentCycle[i]) digitalWrite(DIMMER_PINS[i], 1);  // на текущем тике включаем
    else if (dimmerCounter == dimmerCurrentCycle[i] - 1) digitalWrite(DIMMER_PINS[i], 0);  // на следующем выключаем
  }
  dimmerCounter--;
}

//-- HELPERS ---------------------------------------------------------------------------------------------

byte brightnessLog(byte value) { 
  // it helps to fix brightness to PWM logarithmic connection
  return int(value + pow(2, value/EXPONENTIAL_SCALE_R) - 1)/2;
}

//-- EEPROM ENCODERS -------------------------------------------------------------------------------------

void loadEncoders() {
  byte address = 0;
  for (byte encoderNum = 0; encoderNum < NELEMS(encoders); encoderNum++) {
    EEPROM.get(address, encoders[encoderNum]->value);
    address += sizeof(encoders[encoderNum]->value);
  }
  lampMenu.encoder.maxValue = modes[lampMode[selectedLamp].value].menuDepth+2;
  lampMenu.encoder.value = 1;
}

void saveEncoders() {
  byte address = 0;
  for (byte encoderNum = 0; encoderNum < NELEMS(encoders); encoderNum++) {
    EEPROM.put(address, encoders[encoderNum]->value);
    address += sizeof(encoders[encoderNum]->value);
  }
}

//-- STEADY... -------------------------------------------------------------------------------------------

void setup() {
  loadEncoders();
  MIDI.setHandleNoteOn(handleNoteOn);
  MIDI.begin(MIDI_CHANNEL_OMNI);
  
  u8g2.begin();
  u8g2.setFlipMode(0);
  u8g2.setDrawColor(1);
  u8g2.setFontDirection(0);
        
  pinMode(ROTARY_BUTTON_PIN, INPUT_PULLUP);
  pinMode(ZERO_PIN, INPUT_PULLUP);
  QDec<ROTARY_PIN_A, ROTARY_PIN_B, true>::begin();
  currentEncoder = &lampMenu.encoder;

  for (byte i = 0; i < DIM_AMOUNT; i++) pinMode(DIMMER_PINS[i], OUTPUT);
  attachInterrupt(digitalPinToInterrupt(ZERO_PIN), zeroCrossISR, FALLING);
  Timer2.enableISR();
  Timer2.setPeriod(39); 
  wdt_enable(WDTO_250MS);
}

//-- GO! --------------------------------------------------------------------------------------------------

void loop()
{
  currentMillis = millis();
  MIDI.read();
  encoderHandle();
  menuHandle();
  for (byte i = 0; i < DIM_AMOUNT; i++) {
    byte value = (*modes[lampMode[i].value].handle)(i);
    dimmer[i] = (value+105)/2; // a bit of magic
  }

  
  if (oledLastUpdateMillis <= lastActionMillis) {
    if (currentMillis - oledLastUpdateMillis > OLED_DEBOUNCE_MS) {
      u8g2.firstPage();
      do {
        if (midiDebugging) {
          drawMidiDebugging();
        } else {
          drawChannel();
          if (menuEditing) {
            drawEditingModeScreen();
          } else {
            drawMenu();
          }
          (*selectedMenuItem->drawing)();
        }
      } while ( u8g2.nextPage() );
      oledLastUpdateMillis = currentMillis;
      saveEncoders();
    }
  } else if ((currentMillis - lastActionMillis) > OLED_SLEEP_MS 
          && (currentMillis - lastActionMillis) < OLED_SLEEP_MS + OLED_DEBOUNCE_MS*3) {
  // after a while prepare oled to sleep and call screen clear during some interval(3 x debounces)
    u8g2.clear();
    if (!midiDebugging) {
      currentEncoder = &lampMenu.encoder;
      menuEditing = false;
    }
  }
  wdt_reset();
}
