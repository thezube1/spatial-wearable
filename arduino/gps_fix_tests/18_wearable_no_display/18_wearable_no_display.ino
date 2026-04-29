// Full Spatial Wearable — RADIOS-OFF + DISPLAY-OFF diagnostic build (18)
// Fork of 17_wearable_radios_off. Same as 17 minus all GC9A01 display code.
//
// REMOVED vs 17:
//   - <SPI.h>, <Adafruit_GFX.h>, <Adafruit_GC9A01A.h>
//   - TFT pin defines (SCK/MOSI/CS/DC/RST)
//   - tft object + all drawing helpers (drawCenteredText, updateDisplay)
//   - Display refresh call from loop()
//   - "Starting..." splash in setup
//
// REMOVED vs 14 (cumulative — same as 17 plus this fork's deletions):
//   - All BLE (NimBLE)
//   - All ESP-NOW
//   - WiFi entirely
//   - GC9A01 round display
//
// KEPT vs 17:
//   - GPS UART on GPIO44/43 (same as sketch 02)
//   - MAX30102 heart rate over I2C
//   - Vibration motor pin (kept declared, never buzzes — no peer source)
//   - NVS preferences (untouched in this build)
//   - 1 Hz GPS status print to Serial in the same format as sketch 02
//
// TEST PLAN:
//   With both radios AND the display gone, the only remaining peripherals
//   are I2C (MAX30102 polled every 20ms) and the GPS UART itself. If 18
//   still doesn't acquire GPS in the same setup where 02 works, suspect
//   either I2C noise (next fork could disable MAX30102) or hardware
//   (antenna placement, brownout, IPEX continuity).
// ============================================================

#include <TinyGPSPlus.h>
#include <Wire.h>
#include "MAX30105.h"
#include <Preferences.h>
#include "esp_mac.h"  // efuse MAC without WiFi

// ---- Pin Definitions ----
#define GPS_RX_PIN 44  // D7
#define GPS_TX_PIN 43  // D6

#define MOTOR_PIN  1   // D0

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

// ---- Objects ----
TinyGPSPlus gps;

// GSV "satellites in view" parsers per constellation (mirrors sketch 02).
TinyGPSCustom gpsInView(gps, "GPGSV", 3);
TinyGPSCustom bdInView(gps, "BDGSV", 3);
TinyGPSCustom glInView(gps, "GLGSV", 3);
TinyGPSCustom gaInView(gps, "GAGSV", 3);

// ---- State ----
unsigned long bootMillis = 0;
unsigned long lastStatus = 0;

bool firstSatLogged = false;
bool firstFixLogged = false;

unsigned long buttonDownSince = 0;
bool buttonLatched = false;

uint8_t myMac[6];
char myDeviceName[10];

Preferences prefs;
const char* PREFS_NAMESPACE = "sw-pair";

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
      Serial.println("[BTN] 5s hold (no-op in this build)");
    }
  } else {
    buttonDownSince = 0;
    buttonLatched = false;
  }
}

int parseInt(const char* s) {
  if (!s || !*s) return 0;
  return atoi(s);
}

// =====================================================
//  SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("==========================================");
  Serial.println("  Spatial Wearable — NO RADIOS / NO TFT (18)");
  Serial.println("  GPS + HR (I2C) + motor + button only.");
  Serial.println("==========================================");

  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

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

  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[GPS] UART OK");

  Serial.println("==========================================");
  Serial.println(" Display also off this run.");
  Serial.println(" If GPS still won't acquire, suspect I2C");
  Serial.println(" noise or hardware (antenna/power).");
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
  updateButton();

  unsigned long now = millis();

  // 1 Hz status print, format matches sketch 02 for direct comparison.
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
}
