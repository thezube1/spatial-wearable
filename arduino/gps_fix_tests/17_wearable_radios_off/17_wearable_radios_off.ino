// Full Spatial Wearable — RADIOS-OFF diagnostic build (17)
// Fork of 14_wearable_peer_location. Combines the disable lists from 15 and
// 16 *and additionally* takes WiFi off the air, so this build runs with no
// 2.4 GHz radio activity at all. Goal: match sketch 02_gps_test's RF
// environment as closely as possible while still using the round display,
// HR sensor, and motor.
//
// REMOVED vs 14:
//   - All BLE (NimBLE init, advertising, scanning, server, every
//     characteristic, every callback class, pairing-mode UI, owner auth).
//   - All ESP-NOW (init, broadcastAddr, send/recv callbacks, periodic send).
//   - WiFi STA mode entirely. WiFi was only ever brought up so ESP-NOW had
//     a channel to live on; it has no reason to exist now.
//
// MAC HANDLING:
//   - 14 read the MAC via esp_wifi_get_mac() which requires WiFi up. Here we
//     use esp_read_mac() from "esp_mac.h" — that pulls the burned-in MAC
//     from efuse without bringing any radio online. Used only for a device
//     name in logs; no functional dependency.
//
// KEPT vs 14:
//   - GPS UART (TinyGPSPlus) on GPIO44/43 — identical setup to sketch 02.
//   - GC9A01 round display, repurposed as a GPS-diagnostic screen.
//   - MAX30102 heart rate.
//   - Vibration motor (kept for parity, but never buzzes because there is
//     no peer distance source).
//   - NVS preferences (read-only, just so previous pairing state isn't
//     accidentally wiped while we test).
//
// TEST PLAN:
//   This is the most aggressive sketch-side intervention. If GPS *still*
//   doesn't acquire here in the same environment where sketch 02 worked,
//   the issue is not radio interference — it's hardware (antenna, power,
//   PCB layout) and we should attack that side next.
// ============================================================

#include <TinyGPSPlus.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include "MAX30105.h"
#include <Preferences.h>
#include <math.h>
#include "esp_mac.h"  // esp_read_mac() — efuse MAC without bringing WiFi up

// ---- Pin Definitions ----
#define GPS_RX_PIN 44  // D7
#define GPS_TX_PIN 43  // D6

#define TFT_SCK   7    // D8
#define TFT_MOSI  9    // D10
#define TFT_CS    2    // D1
#define TFT_DC    4    // D3
#define TFT_RST   3    // D2

#define MOTOR_PIN 1    // D0

#define BUTTON_PIN     0
#define BUTTON_HOLD_MS 5000UL

// ---- Heart Rate (MAX30102) ----
MAX30105 particleSensor;
bool maxFound = false;

#define HR_SAMPLE_INTERVAL  20
#define HR_BUFFER_SIZE      75
#define HR_MIN_BPM          40
#define HR_MAX_BPM          200
#define HR_FINGER_THRESHOLD 50000

long irBuffer[HR_BUFFER_SIZE];
int irBufIdx = 0;
bool irBufFull = false;
unsigned long lastHrSample = 0;
int currentBPM = 0;
bool fingerDetected = false;

// ---- Haptic ----
// Kept for parity with 14/15/16. Never fires here because there's no peer
// distance source — getActiveDistance() always returns -1.
#define HAPTIC_RANGE_M      10.0
#define HAPTIC_PULSE_MS     200
unsigned long lastHapticOn = 0;
bool hapticOn = false;

// ---- Objects ----
Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);
TinyGPSPlus gps;

// GSV "satellites in view" parsers per constellation (same as sketch 02).
TinyGPSCustom gpsInView(gps, "GPGSV", 3);
TinyGPSCustom bdInView(gps, "BDGSV", 3);
TinyGPSCustom glInView(gps, "GLGSV", 3);
TinyGPSCustom gaInView(gps, "GAGSV", 3);

// ---- State ----
unsigned long bootMillis = 0;
unsigned long lastStatus = 0;
unsigned long lastDisplayUpdate = 0;

bool firstSatLogged = false;
bool firstFixLogged = false;

unsigned long buttonDownSince = 0;
bool buttonLatched = false;

// MAC just for the device label in logs.
uint8_t myMac[6];
char myDeviceName[10];

// NVS — kept available so we don't accidentally clobber pairing data, but
// we don't actually use the values for anything in this build.
Preferences prefs;
const char* PREFS_NAMESPACE = "sw-pair";

// ---- Colors ----
#define COLOR_BG     0x0000
#define COLOR_GREEN  0x07E0
#define COLOR_YELLOW 0xFFE0
#define COLOR_ORANGE 0xFD20
#define COLOR_RED    0xF800
#define COLOR_GRAY   0x7BEF
#define COLOR_CYAN   0x07FF
#define COLOR_WHITE  0xFFFF
#define COLOR_BLUE   0x001F

// =====================================================
//  HEART RATE
// =====================================================

void sampleHeartRate() {
  if (!maxFound) return;
  if (millis() - lastHrSample < HR_SAMPLE_INTERVAL) return;
  lastHrSample = millis();

  long ir = particleSensor.getIR();
  fingerDetected = (ir > HR_FINGER_THRESHOLD);

  if (!fingerDetected) {
    currentBPM = 0;
    irBufIdx = 0;
    irBufFull = false;
    return;
  }

  irBuffer[irBufIdx] = ir;
  irBufIdx++;
  if (irBufIdx >= HR_BUFFER_SIZE) {
    irBufIdx = 0;
    irBufFull = true;
  }
  if (!irBufFull) return;

  int count = HR_BUFFER_SIZE;
  long irSum = 0;
  for (int i = 0; i < count; i++) irSum += irBuffer[i];
  long irMean = irSum / count;

  int beats = 0;
  bool wasAbove = false;
  for (int i = 0; i < count; i++) {
    bool above = irBuffer[i] > irMean;
    if (above && !wasAbove) beats++;
    wasAbove = above;
  }

  float durationSec = (count * HR_SAMPLE_INTERVAL) / 1000.0;
  int bpm = (int)(beats / durationSec * 60.0);
  if (bpm >= HR_MIN_BPM && bpm <= HR_MAX_BPM) {
    currentBPM = bpm;
  }
}

// =====================================================
//  HAPTIC (kept for parity — never fires, no peer source)
// =====================================================

void updateHaptic() {
  if (hapticOn) {
    digitalWrite(MOTOR_PIN, LOW);
    hapticOn = false;
  }
}

// =====================================================
//  BUTTON
// =====================================================

void updateButton() {
  int level = digitalRead(BUTTON_PIN);
  unsigned long now = millis();
  if (level == LOW) {
    if (buttonDownSince == 0) {
      buttonDownSince = now;
    } else if (!buttonLatched && (now - buttonDownSince >= BUTTON_HOLD_MS)) {
      buttonLatched = true;
      Serial.println("[BTN] 5s hold (no-op in radios-off build)");
    }
  } else {
    buttonDownSince = 0;
    buttonLatched = false;
  }
}

// =====================================================
//  DISPLAY HELPERS
// =====================================================

void drawCenteredText(const char* text, int y, uint16_t color, uint8_t size) {
  tft.setTextSize(size);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  tft.setTextColor(color, COLOR_BG);
  tft.setCursor((240 - w) / 2, y);
  tft.print(text);
}

int parseInt(const char* s) {
  if (!s || !*s) return 0;
  return atoi(s);
}

// Diagnostic GPS-focused screen. Refreshes every 1s.
void updateDisplay() {
  tft.fillScreen(COLOR_BG);

  // Header
  drawCenteredText("GPS DIAG 17", 18, COLOR_CYAN, 1);
  drawCenteredText("(no radios)", 32, COLOR_GRAY, 1);

  // Satellite count — big, center
  int viewGps = parseInt(gpsInView.value());
  int viewBd  = parseInt(bdInView.value());
  int viewGl  = parseInt(glInView.value());
  int viewGa  = parseInt(gaInView.value());
  int viewTot = viewGps + viewBd + viewGl + viewGa;
  int used    = gps.satellites.isValid() ? gps.satellites.value() : 0;
  bool hasFix = gps.location.isValid() && gps.location.age() < 5000;

  uint16_t stateColor;
  const char* state;
  if (hasFix) {
    state = "FIX";
    stateColor = COLOR_GREEN;
  } else if (viewTot > 0) {
    state = "ACQ";
    stateColor = COLOR_YELLOW;
  } else {
    state = "---";
    stateColor = COLOR_RED;
  }
  drawCenteredText(state, 60, stateColor, 4);

  char satBuf[24];
  snprintf(satBuf, sizeof(satBuf), "view %d / used %d", viewTot, used);
  drawCenteredText(satBuf, 105, COLOR_WHITE, 1);

  char perCon[24];
  snprintf(perCon, sizeof(perCon), "G%d B%d L%d A%d",
           viewGps, viewBd, viewGl, viewGa);
  drawCenteredText(perCon, 120, COLOR_GRAY, 1);

  // Lat/lng (if fixed)
  if (hasFix) {
    char latBuf[20], lngBuf[20];
    snprintf(latBuf, sizeof(latBuf), "%.5f", gps.location.lat());
    snprintf(lngBuf, sizeof(lngBuf), "%.5f", gps.location.lng());
    drawCenteredText(latBuf, 142, COLOR_WHITE, 1);
    drawCenteredText(lngBuf, 154, COLOR_WHITE, 1);
  } else {
    drawCenteredText("no fix", 148, COLOR_GRAY, 1);
  }

  // HR (if finger detected)
  if (maxFound) {
    char hrBuf[20];
    if (fingerDetected && currentBPM > 0) {
      snprintf(hrBuf, sizeof(hrBuf), "%d BPM", currentBPM);
      drawCenteredText(hrBuf, 178, COLOR_RED, 2);
    } else if (fingerDetected) {
      drawCenteredText("Reading...", 184, COLOR_ORANGE, 1);
    } else {
      drawCenteredText("No finger", 184, COLOR_GRAY, 1);
    }
  }

  // Elapsed time
  char tBuf[20];
  unsigned long elapsed = (millis() - bootMillis) / 1000;
  snprintf(tBuf, sizeof(tBuf), "t=%lus", elapsed);
  drawCenteredText(tBuf, 210, COLOR_GRAY, 1);
}

// =====================================================
//  SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("==========================================");
  Serial.println("  Spatial Wearable — RADIOS-OFF (17)");
  Serial.println("  No BLE, no ESP-NOW, no WiFi. GPS only.");
  Serial.println("==========================================");

  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Read MAC from efuse — does NOT bring WiFi up.
  esp_read_mac(myMac, ESP_MAC_WIFI_STA);
  snprintf(myDeviceName, sizeof(myDeviceName), "SW-%02X%02X",
           myMac[4], myMac[5]);
  Serial.printf("Device: %s (MAC %02X:%02X:%02X:%02X:%02X:%02X)\n",
                myDeviceName,
                myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);

  Wire.begin(5, 6);
  Serial.print("[MAX30102] ");
  if (particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    particleSensor.setup(0x1F, 4, 2, 100, 411, 4096);
    maxFound = true;
    Serial.println("OK");
  } else {
    Serial.println("NOT FOUND — HR disabled");
  }

  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(COLOR_BG);
  drawCenteredText("Starting...", 110, COLOR_YELLOW, 2);

  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[GPS] UART OK");

  Serial.println("==========================================");
  Serial.println(" RADIOS OFF: no BLE, no ESP-NOW, no WiFi.");
  Serial.println(" If GPS still won't acquire here, the");
  Serial.println(" issue is hardware (antenna/power/layout).");
  Serial.println("==========================================\n");

  bootMillis = millis();
  lastStatus = millis();
}

// =====================================================
//  LOOP
// =====================================================

void loop() {
  while (Serial1.available()) {
    gps.encode(Serial1.read());
  }

  sampleHeartRate();
  updateHaptic();
  updateButton();

  unsigned long now = millis();

  // 1 Hz status print (mirrors sketch 02 format so logs are directly
  // comparable when troubleshooting).
  if (now - lastStatus >= 1000) {
    lastStatus = now;
    unsigned long elapsed = (now - bootMillis) / 1000;

    int viewGps = parseInt(gpsInView.value());
    int viewBd  = parseInt(bdInView.value());
    int viewGl  = parseInt(glInView.value());
    int viewGa  = parseInt(gaInView.value());
    int viewTot = viewGps + viewBd + viewGl + viewGa;
    int used    = gps.satellites.isValid() ? gps.satellites.value() : 0;
    bool hasFix = gps.location.isValid() && gps.location.age() < 5000;

    if (viewTot > 0 && !firstSatLogged) {
      firstSatLogged = true;
      Serial.printf("\n>>> First satellite at t=%lus <<<\n\n", elapsed);
    }
    if (hasFix && !firstFixLogged) {
      firstFixLogged = true;
      Serial.printf("\n*** FIRST FIX at t=%lus ***\n\n", elapsed);
    }

    Serial.printf("[t=%4lus] %s  view: G=%d B=%d L=%d A=%d (tot %d) | used=%d",
                  elapsed,
                  hasFix ? "FIX" : (viewTot > 0 ? "ACQ" : "---"),
                  viewGps, viewBd, viewGl, viewGa, viewTot, used);
    if (gps.hdop.isValid() && gps.hdop.hdop() < 25.0) {
      Serial.printf(" | HDOP %.1f", gps.hdop.hdop());
    }
    Serial.println();

    if (hasFix) {
      Serial.printf("           lat %.6f  lng %.6f  alt %.1fm\n",
                    gps.location.lat(), gps.location.lng(),
                    gps.altitude.isValid() ? gps.altitude.meters() : 0.0);
    }

    Serial.printf("           [chars %lu | sentences ok %lu | fail %lu | BPM %d]\n",
                  gps.charsProcessed(),
                  gps.passedChecksum(),
                  gps.failedChecksum(),
                  currentBPM);
  }

  // Display refresh every 1s.
  if (now - lastDisplayUpdate >= 1000) {
    lastDisplayUpdate = now;
    updateDisplay();
  }
}
