#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// ============================================================
// DAEDRIX
// Resistor / Diode / LED Analyzer
// STM32F103C8T6
// ============================================================

// ============================================================
// OLED
// ============================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C

#define SCL_PIN PB6
#define SDA_PIN PB7

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ============================================================
// DRIVE PINS
// ============================================================

const int PIN_A = PA1;
const int PIN_B = PA2;

// ============================================================
// ADC SENSE PINS
// ============================================================

const int SENSE_A = PA3;
const int SENSE_B = PA4;

// ============================================================
// CIRCUIT CONSTANTS
// ============================================================

const float R_A = 1000.0;
const float R_B = 1000.0;

const float VCC = 3.3;
const float ADC_MAX = 4095.0;

const int ADC_SAMPLES = 16;

// Below this current, we treat the socket as empty. Named here
// instead of scattered as a magic number, since it's used in
// both detection and presence-check logic and needs to stay
// consistent between the two.
const float PRESENCE_CURRENT_THRESHOLD = 0.00002; // 20 microamps

// How many settle time we give the divider before sampling.
// Bumped slightly from the original 10ms — cheap insurance
// against borderline high-resistance readings landing right on
// the noise floor.
const int SETTLE_TIME_MS = 15;

// ============================================================
// TIMING
// ============================================================

const unsigned long FRAME_TIME = 40;
const unsigned long SCAN_INTERVAL = 300;

// Same cadence used everywhere presence gets polled — IDLE,
// CREDITS, and RESULT all check on this interval now, instead
// of IDLE/CREDITS hammering it every single loop() call.
const unsigned long PRESENCE_CHECK_INTERVAL = 200;

// Component must fail this many CONSECUTIVE presence checks
// before we declare it removed. This is the actual fix for the
// flicker: a single noisy sample can no longer bounce the UI
// back to IDLE — it takes a sustained run of low readings.
const int PRESENCE_MISS_LIMIT = 4;

// Credits
const unsigned long CREDITS_DELAY = 15000;

// ============================================================
// TIMERS
// ============================================================

unsigned long lastFrame = 0;
unsigned long lastScan = 0;
unsigned long lastPresenceCheck = 0;

unsigned long idleStart = 0;

int presenceMissCount = 0;

// ============================================================
// STATES
// ============================================================

enum AppState {
  IDLE,
  CREDITS,
  DETECTING,
  RESULT
};

AppState state = IDLE;

// ============================================================
// CREDITS
// ============================================================

const char* credits[] = {
  "DAEDRIX", "",
  "A COMPONENT", "ANALYZER", "",
  "MADE BY", "",
  "Sreegovind P", "Shravan PD", "Sooraj Sunilkumar", "Midhun M", "", "",
};

const int creditCount = sizeof(credits) / sizeof(credits[0]);
int creditOffset = SCREEN_HEIGHT;

// ============================================================
// MEASUREMENT RESULT
// ============================================================

struct Measurement {
  float va;
  float vb;
  float current;
  float vf;
  float resistance;
  bool conducting;
};

// ============================================================
// COMPONENT TYPE
// ============================================================

enum ComponentType {
  TYPE_NONE,
  TYPE_RESISTOR,
  TYPE_DIODE,
  TYPE_LED,
  TYPE_UNKNOWN
};

ComponentType currentType = TYPE_NONE;

// ============================================================
// STORED RESULT
// ============================================================

float resultValue = 0.0;
float resultCurrent = 0.0;
float resultVf = 0.0;
float resultVA = 0.0;
float resultVB = 0.0;
bool resultReverse = false;

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);

  Wire.setSCL(SCL_PIN);
  Wire.setSDA(SDA_PIN);
  Wire.begin();

  delay(100);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    while (true);
  }

  analogReadResolution(12);

  pinMode(PIN_A, INPUT_ANALOG);
  pinMode(PIN_B, INPUT_ANALOG);
  pinMode(SENSE_A, INPUT_ANALOG);
  pinMode(SENSE_B, INPUT_ANALOG);

  display.clearDisplay();
  display.display();

  idleStart = millis();

  Serial.println();
  Serial.println("==============================");
  Serial.println("         DAEDRIX");
  Serial.println("   COMPONENT ANALYZER");
  Serial.println("==============================");
  Serial.println("DRIVE A : PA1");
  Serial.println("DRIVE B : PA2");
  Serial.println("SENSE A : PA3");
  Serial.println("SENSE B : PA4");
  Serial.println("R_A     : 1000 ohm");
  Serial.println("R_B     : 1000 ohm");
  Serial.println();
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
  unsigned long now = millis();

  switch (state) {

    // ========================================================
    // IDLE
    // ========================================================
    case IDLE:

      if (now - lastPresenceCheck >= PRESENCE_CHECK_INTERVAL) {
        lastPresenceCheck = now;

        if (checkForComponent()) {
          state = DETECTING;
          lastScan = 0;
          lastFrame = 0;
          presenceMissCount = 0;
          break;
        }
      }

      if (now - idleStart >= CREDITS_DELAY) {
        state = CREDITS;
        creditOffset = SCREEN_HEIGHT;
        break;
      }

      if (now - lastFrame >= FRAME_TIME) {
        lastFrame = now;
        drawIdle();
      }

      break;

    // ========================================================
    // CREDITS
    // ========================================================
    case CREDITS:

      if (now - lastPresenceCheck >= PRESENCE_CHECK_INTERVAL) {
        lastPresenceCheck = now;

        if (checkForComponent()) {
          state = DETECTING;
          lastScan = 0;
          lastFrame = 0;
          presenceMissCount = 0;
          break;
        }
      }

      if (now - lastFrame >= FRAME_TIME) {
        lastFrame = now;
        drawCredits();
      }

      break;

    // ========================================================
    // DETECTING
    // ========================================================
    case DETECTING:

      if (now - lastFrame >= FRAME_TIME) {
        lastFrame = now;
        drawScanning();
      }

      if (now - lastScan >= SCAN_INTERVAL) {
        lastScan = now;

        bool found = detectComponent();

        if (found) {
          presenceMissCount = 0;
          lastPresenceCheck = now;
          state = RESULT;
          lastFrame = 0;
        }
      }

      break;

    // ========================================================
    // RESULT
    // ========================================================
    case RESULT:

      if (now - lastFrame >= FRAME_TIME) {
        lastFrame = now;

        switch (currentType) {
          case TYPE_RESISTOR:
            drawResistorResult(resultValue, resultCurrent, resultVA, resultVB);
            break;
          case TYPE_DIODE:
            drawDiodeResult(resultValue, resultCurrent, resultReverse);
            break;
          case TYPE_LED:
            drawLEDResult(resultValue, resultCurrent, resultReverse);
            break;
          default:
            drawUnknownResult();
            break;
        }
      }

      // --------------------------------------------------------
      // Presence check with consecutive-miss debounce.
      // This is the actual flicker fix: a single noisy low
      // sample no longer matters. Only a sustained run of
      // PRESENCE_MISS_LIMIT bad readings in a row triggers
      // a return to IDLE.
      // --------------------------------------------------------

      if (now - lastPresenceCheck >= PRESENCE_CHECK_INTERVAL) {
        lastPresenceCheck = now;

        bool stillPresent = checkForComponent();

        if (stillPresent) {
          presenceMissCount = 0;
        } else {
          presenceMissCount++;

          if (presenceMissCount >= PRESENCE_MISS_LIMIT) {
            releasePins();
            state = IDLE;
            idleStart = now;
            lastFrame = 0;
            presenceMissCount = 0;
          }
        }
      }

      break;
  }
}

// ============================================================
// ADC VOLTAGE
// ============================================================

float readVoltage(int pin) {
  analogRead(pin); // discard first conversion

  long total = 0;

  for (int i = 0; i < ADC_SAMPLES; i++) {
    total += analogRead(pin);
    delayMicroseconds(100);
  }

  float raw = (float)total / ADC_SAMPLES;
  return (raw / ADC_MAX) * VCC;
}

// ============================================================
// RELEASE DRIVE PINS
// ============================================================

void releasePins() {
  pinMode(PIN_A, INPUT_ANALOG);
  pinMode(PIN_B, INPUT_ANALOG);
}

// ============================================================
// FORWARD MEASUREMENT
// 3.3V -> PA1 -> 1k -> A -> DUT -> B -> 1k -> PA2 -> GND
// ============================================================

Measurement measureForward() {
  Measurement m;

  pinMode(PIN_A, OUTPUT);
  digitalWrite(PIN_A, HIGH);

  pinMode(PIN_B, OUTPUT);
  digitalWrite(PIN_B, LOW);

  delay(SETTLE_TIME_MS);

  m.va = readVoltage(SENSE_A);
  m.vb = readVoltage(SENSE_B);

  m.current = m.vb / R_B;
  m.vf = m.va - m.vb;

  m.resistance = (m.current > 0.000001) ? (m.vf / m.current) : -1;
  m.conducting = (m.vb > 0.02 && m.vf > 0.02);

  return m;
}

// ============================================================
// REVERSE MEASUREMENT
// 3.3V -> PA2 -> 1k -> B -> DUT -> A -> 1k -> PA1 -> GND
// ============================================================

Measurement measureReverse() {
  Measurement m;

  pinMode(PIN_B, OUTPUT);
  digitalWrite(PIN_B, HIGH);

  pinMode(PIN_A, OUTPUT);
  digitalWrite(PIN_A, LOW);

  delay(SETTLE_TIME_MS);

  float highNode = readVoltage(SENSE_B);
  float lowNode = readVoltage(SENSE_A);

  m.va = lowNode;
  m.vb = highNode;

  m.current = lowNode / R_A;
  m.vf = highNode - lowNode;

  m.resistance = (m.current > 0.000001) ? (m.vf / m.current) : -1;
  m.conducting = (lowNode > 0.02 && m.vf > 0.02);

  return m;
}

// ============================================================
// COMPONENT PRESENCE
// ============================================================

bool checkForComponent() {
  Measurement f = measureForward();
  releasePins();
  bool forwardPath = f.current > PRESENCE_CURRENT_THRESHOLD;

  Measurement r = measureReverse();
  releasePins();
  bool reversePath = r.current > PRESENCE_CURRENT_THRESHOLD;

  return forwardPath || reversePath;
}

// ============================================================
// COMPONENT DETECTION
// ============================================================

bool detectComponent() {
  Serial.println();
  Serial.println("------------------------------");

  Measurement f = measureForward();
  releasePins();

  Serial.println("FORWARD");
  Serial.print("TEST A : "); Serial.print(f.va, 3); Serial.println(" V");
  Serial.print("TEST B : "); Serial.print(f.vb, 3); Serial.println(" V");
  Serial.print("Current: "); Serial.print(f.current * 1000.0, 3); Serial.println(" mA");
  Serial.print("DUT Vf : "); Serial.print(f.vf, 3); Serial.println(" V");

  Measurement r = measureReverse();
  releasePins();

  Serial.println();
  Serial.println("REVERSE");
  Serial.print("TEST A : "); Serial.print(r.va, 3); Serial.println(" V");
  Serial.print("TEST B : "); Serial.print(r.vb, 3); Serial.println(" V");
  Serial.print("Current: "); Serial.print(r.current * 1000.0, 3); Serial.println(" mA");
  Serial.print("DUT Vf : "); Serial.print(r.vf, 3); Serial.println(" V");

  bool forwardConducting = f.current > PRESENCE_CURRENT_THRESHOLD;
  bool reverseConducting = r.current > PRESENCE_CURRENT_THRESHOLD;

  // ==========================================================
  // NOTHING CONNECTED
  // ==========================================================
  if (!forwardConducting && !reverseConducting) {
    Serial.println("TYPE: NO COMPONENT");
    currentType = TYPE_NONE;
    return false;
  }

  // ==========================================================
  // RESISTOR
  // ==========================================================
  if (forwardConducting && reverseConducting) {
    float resistance = f.resistance;

    Serial.println("TYPE: RESISTOR");
    Serial.print("Resistance: "); Serial.print(resistance, 1); Serial.println(" ohm");

    currentType = TYPE_RESISTOR;
    resultValue = resistance;
    resultCurrent = f.current;
    resultVf = f.vf;
    resultVA = f.va;
    resultVB = f.vb;
    resultReverse = false;

    if (resistance < 5.0 || resistance > 1000000.0) {
      Serial.println("WARNING: OUT OF TRUSTED RANGE");
      currentType = TYPE_UNKNOWN;
      drawUnknownResult();
      return true;
    }

    drawResistorResult(resistance, f.current, f.va, f.vb);
    return true;
  }

  // ==========================================================
  // FORWARD DIODE / LED
  // ==========================================================
  if (forwardConducting && !reverseConducting) {
    float vf = f.vf;

    Serial.println("FORWARD CONDUCTION");

    resultVf = vf;
    resultCurrent = f.current;
    resultReverse = false;

    if (vf >= 0.20 && vf < 1.20) {
      Serial.println("TYPE: DIODE");
      currentType = TYPE_DIODE;
      resultValue = vf;
      drawDiodeResult(vf, f.current, false);
      return true;
    }

    if (vf >= 1.20 && vf <= 3.30) {
      Serial.println("TYPE: LED");
      currentType = TYPE_LED;
      resultValue = vf;
      drawLEDResult(vf, f.current, false);
      return true;
    }

    Serial.println("TYPE: UNKNOWN");
    currentType = TYPE_UNKNOWN;
    resultValue = vf;
    drawUnknownResult();
    return true;
  }

  // ==========================================================
  // REVERSE DIODE / LED
  // ==========================================================
  if (!forwardConducting && reverseConducting) {
    float vf = r.vf;

    Serial.println("REVERSE POLARITY COMPONENT");

    resultVf = vf;
    resultCurrent = r.current;
    resultReverse = true;

    if (vf >= 0.20 && vf < 1.20) {
      Serial.println("TYPE: DIODE");
      currentType = TYPE_DIODE;
      resultValue = vf;
      drawDiodeResult(vf, r.current, true);
      return true;
    }

    if (vf >= 1.20 && vf <= 3.30) {
      Serial.println("TYPE: LED");
      currentType = TYPE_LED;
      resultValue = vf;
      drawLEDResult(vf, r.current, true);
      return true;
    }

    Serial.println("TYPE: UNKNOWN");
    currentType = TYPE_UNKNOWN;
    resultValue = vf;
    drawUnknownResult();
    return true;
  }

  return false;
}

// ============================================================
// SHARED UI HELPERS
// ============================================================

// Thin outer frame used consistently across every non-idle
// screen, so the whole device reads as one coherent product
// rather than a stack of differently-styled screens.
void drawFrame() {
  display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
}

// Header bar with title + a small breathing "live" dot.
// The dot's only job is to visually confirm the screen is
// still actively updating — genuinely useful now that presence
// checks are debounced and won't cause visible jumps anymore.
void resultHeader(const char* title) {
  display.clearDisplay();
  drawFrame();

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(6, 3);
  display.print(title);

  bool pulseOn = ((millis() / 300) % 2) == 0;
  if (pulseOn) {
    display.fillCircle(120, 5, 2, SSD1306_WHITE);
  } else {
    display.drawCircle(120, 5, 2, SSD1306_WHITE);
  }

  display.drawLine(4, 13, 123, 13, SSD1306_WHITE);
}

// Bottom info bar: divider line + left-aligned label,
// right-aligned value. Reused by every result screen instead
// of each one hand-placing scattered text.
void drawInfoBar(String left, String right) {
  display.drawLine(4, 50, 123, 50, SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(6, 54);
  display.print(left);

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(right.c_str(), 0, 0, &x1, &y1, &w, &h);
  display.setCursor(122 - w, 54);
  display.print(right);
}

// ============================================================
// RESISTOR SYMBOL
// ============================================================

void drawResistorSymbol(int y, bool animated) {
  int left = 10;
  int right = 118;

  display.drawLine(left, y, 25, y, SSD1306_WHITE);
  display.drawLine(25, y, 31, y - 5, SSD1306_WHITE);
  display.drawLine(31, y - 5, 37, y + 5, SSD1306_WHITE);
  display.drawLine(37, y + 5, 43, y - 5, SSD1306_WHITE);
  display.drawLine(43, y - 5, 49, y + 5, SSD1306_WHITE);
  display.drawLine(49, y + 5, 55, y - 5, SSD1306_WHITE);
  display.drawLine(55, y - 5, 61, y, SSD1306_WHITE);
  display.drawLine(61, y, right, y, SSD1306_WHITE);

  if (animated) {
    int pos = 12 + ((millis() / 45) % 100);
    display.fillCircle(pos, y, 1, SSD1306_WHITE);
  }
}

// ============================================================
// DIODE SYMBOL
// ============================================================

void drawDiodeSymbol(int y, bool animated) {
  display.drawLine(10, y, 47, y, SSD1306_WHITE);
  display.drawLine(47, y, 61, y - 8, SSD1306_WHITE);
  display.drawLine(47, y, 61, y + 8, SSD1306_WHITE);
  display.drawLine(61, y - 8, 61, y + 8, SSD1306_WHITE);
  display.drawLine(61, y, 118, y, SSD1306_WHITE);
  display.drawLine(65, y - 8, 65, y + 8, SSD1306_WHITE);

  if (animated) {
    int pos = 14 + ((millis() / 50) % 95);
    display.fillCircle(pos, y, 1, SSD1306_WHITE);
  }
}

// ============================================================
// LED SYMBOL
// ============================================================

void drawLEDSymbol(int y) {
  display.drawLine(10, y, 47, y, SSD1306_WHITE);
  display.drawLine(47, y, 61, y - 8, SSD1306_WHITE);
  display.drawLine(47, y, 61, y + 8, SSD1306_WHITE);
  display.drawLine(61, y - 8, 61, y + 8, SSD1306_WHITE);
  display.drawLine(61, y, 118, y, SSD1306_WHITE);
  display.drawLine(65, y - 8, 65, y + 8, SSD1306_WHITE);

  float t = millis() / 180.0;
  int pulse = (int)(sin(t) * 2.0);

  display.drawLine(50, y - 11, 42, y - 19 - pulse, SSD1306_WHITE);
  display.drawLine(57, y - 12, 54, y - 22 + pulse, SSD1306_WHITE);
  display.drawLine(64, y - 11, 69, y - 19 - pulse, SSD1306_WHITE);
}

// ============================================================
// SCANNING SCREEN
// ============================================================

void drawScanning() {
  display.clearDisplay();
  drawFrame();

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(6, 3);
  display.print("DAEDRIX");
  display.setCursor(75, 3);
  display.print("ANALYZING");

  display.drawLine(4, 13, 123, 13, SSD1306_WHITE);

  int phase = (millis() / 900) % 3;

  if (phase == 0) {
    drawResistorSymbol(27, true);
  } else if (phase == 1) {
    drawDiodeSymbol(27, true);
  } else {
    drawLEDSymbol(27);
  }

  display.setCursor(25, 38);
  display.print("TESTING");

  int dots = (millis() / 250) % 4;
  for (int i = 0; i < dots; i++) {
    display.print(".");
  }

  display.drawRect(10, 51, 108, 8, SSD1306_WHITE);
  int progress = 12 + ((millis() / 20) % 104);
  display.fillRect(12, 53, progress - 12, 4, SSD1306_WHITE);

  display.display();
}

// ============================================================
// RESISTOR RESULT
// ============================================================

void drawResistorResult(float resistance, float current, float va, float vb) {
  resultHeader("RESISTOR");
  drawResistorSymbol(24, true);

  display.setTextSize(2);
  display.setCursor(6, 32);

  String valueStr;
  if (resistance >= 1000000.0) {
    valueStr = String(resistance / 1000000.0, 2) + "M";
  } else if (resistance >= 1000.0) {
    valueStr = String(resistance / 1000.0, 2) + "K";
  } else {
    valueStr = String(resistance, 1) + "R";
  }
  display.print(valueStr);

  display.setTextSize(1);
  display.setCursor(96, 34);
  display.print(current * 1000.0, 1);
  display.print("m");

  char buf[16];
  snprintf(buf, sizeof(buf), "A:%.2fV", va);
  String leftInfo = String(buf);
  snprintf(buf, sizeof(buf), "B:%.2fV", vb);
  String rightInfo = String(buf);

  drawInfoBar(leftInfo, rightInfo);
  display.display();
}

// ============================================================
// DIODE RESULT
// ============================================================

void drawDiodeResult(float vf, float current, bool reverse) {
  resultHeader("DIODE");
  drawDiodeSymbol(24, true);

  display.setTextSize(2);
  display.setCursor(6, 32);
  display.print(vf, 2);
  display.print("V");

  display.setTextSize(1);
  display.setCursor(96, 34);
  display.print(current * 1000.0, 1);
  display.print("m");

  String biasLabel = reverse ? "BIAS: REV" : "BIAS: FWD";
  drawInfoBar(biasLabel, "TYP 0.5-1.0V");

  display.display();
}

// ============================================================
// LED RESULT
// ============================================================

void drawLEDResult(float vf, float current, bool reverse) {
  resultHeader("LED");
  drawLEDSymbol(24);

  display.setTextSize(2);
  display.setCursor(6, 32);
  display.print(vf, 2);
  display.print("V");

  display.setTextSize(1);
  display.setCursor(96, 34);
  display.print(current * 1000.0, 1);
  display.print("m");

  String biasLabel = reverse ? "BIAS: REV" : "BIAS: FWD";
  drawInfoBar(biasLabel, "TYP 1.5-2.8V");

  display.display();
}

// ============================================================
// UNKNOWN RESULT
// Now diagnostic instead of just a "?" — shows the actual
// measured values so you can see why it didn't classify,
// rather than hitting a dead end.
// ============================================================

void drawUnknownResult() {
  resultHeader("UNKNOWN");

  display.setTextSize(1);
  display.setCursor(6, 20);
  display.print("OUT OF RANGE");

  display.setTextSize(2);
  display.setCursor(96, 18);
  display.print("?");

  char buf[16];
  snprintf(buf, sizeof(buf), "VF:%.2fV", resultVf);
  String leftInfo = String(buf);
  snprintf(buf, sizeof(buf), "I:%.2fmA", resultCurrent * 1000.0);
  String rightInfo = String(buf);

  display.setTextSize(1);
  display.setCursor(6, 36);
  display.print(leftInfo);
  display.setCursor(6, 46);
  display.print(rightInfo);

  drawInfoBar("CHECK LEADS", "RETRY");
  display.display();
}

// ============================================================
// IDLE SCREEN
// ============================================================

void drawIdle() {
  display.clearDisplay();

  display.drawRect(1, 1, 126, 62, SSD1306_WHITE);
  display.drawRect(4, 4, 120, 56, SSD1306_WHITE);

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);

  const char* title = "DAEDRIX";
  int16_t x1, y1;
  uint16_t w, h;

  display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 9);
  display.println(title);

  display.drawLine(13, 28, 114, 28, SSD1306_WHITE);

  display.setTextSize(1);
  const char* action = "INSERT COMPONENT";
  display.getTextBounds(action, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 34);
  display.println(action);

  drawWave();
  display.display();
}

// ============================================================
// IDLE WAVE
// ============================================================

void drawWave() {
  float t = millis() / 180.0;
  int previousX = 10;
  int previousY = 53;

  for (int x = 10; x <= 117; x += 2) {
    float wave = sin((x * 0.22) + t) * 3.0;
    int y = 53 + (int)wave;
    display.drawLine(previousX, previousY, x, y, SSD1306_WHITE);
    previousX = x;
    previousY = y;
  }

  int dotX = 10 + ((millis() / 30) % 108);
  display.fillCircle(dotX, 53, 1, SSD1306_WHITE);
}

// ============================================================
// CREDITS
// ============================================================

void drawCredits() {
  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  int y = creditOffset;

  for (int i = 0; i < creditCount; i++) {
    const char* line = credits[i];

    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(line, 0, 0, &x1, &y1, &w, &h);

    int x = (128 - w) / 2;
    display.setCursor(x, y);
    display.println(line);

    y += 10;
  }

  display.display();

  creditOffset--;

  if (creditOffset < -(creditCount * 10)) {
    state = IDLE;
    idleStart = millis();
  }
}
