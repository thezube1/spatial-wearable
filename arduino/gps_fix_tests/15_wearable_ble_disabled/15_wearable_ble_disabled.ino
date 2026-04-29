// Full Spatial Wearable — BLE-DISABLED diagnostic build (15)
// Fork of 14_wearable_peer_location. Strips out all NimBLE functionality to
// isolate whether the 2.4 GHz BLE radio is desensitizing the GPS receiver.
//
// Plan: if 15 acquires GPS the way 02_gps_test does, BLE was the blocker.
// If not, fork again and disable WiFi/ESP-NOW next, then display, etc.,
// until we match the conditions in which 02_ works.
//
// REMOVED vs 14:
//   - <NimBLEDevice.h> + every advertising/scanning/server/characteristic call
//   - All BLE callback classes (server, scan, owner-write, owner-auth,
//     target-write, MAC read)
//   - RSSI smoothing, rssiToDistance, signal-bar UI
//   - startBLE / stopBLE / restartBleScan / restartBLEFull / updateBleRetry
//   - notifyLocationIfDue / notifyPeerLocationIfDue (no BLE central anyway)
//   - Pairing mode (full-screen UI) and AUTH_GRACE_MS timeout
//   - BLE branch of the mode switcher; mode is locked to MODE_GPS
//
// KEPT vs 14:
//   - GPS UART (TinyGPSPlus) on GPIO44/43 — same as sketch 02
//   - ESP-NOW broadcast + filtered receive (target loaded from NVS if present)
//   - WiFi STA brought up only because ESP-NOW needs it
//   - MAX30102 heart rate
//   - GC9A01 round display (GPS-only UI)
//   - Vibration motor / haptic
//   - GPIO0 5-second hold — now clears NVS target instead of entering pairing
//
// ----- original header (14) below -----
// Full Spatial Wearable — Peer Location build (14)
// Adds peer-location BLE notify (…abd3) so the iOS map can render the tracked
// peer alongside the wearable's own location.
// (Full original 14 header removed for brevity.)
// ============================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <TinyGPSPlus.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include "MAX30105.h"
#include <Preferences.h>
#include <math.h>

// ---- Pin Definitions ----
#define GPS_RX_PIN 44  // D7
#define GPS_TX_PIN 43  // D6

#define TFT_SCK   7    // D8
#define TFT_MOSI  9    // D10
#define TFT_CS    2    // D1
#define TFT_DC    4    // D3
#define TFT_RST   3    // D2

#define MOTOR_PIN 1    // D0

#define BUTTON_PIN      0
#define BUTTON_HOLD_MS  5000UL

// ---- ESP-NOW ----
uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Kept only so the device still has a recognizable name in serial logs and
// matches the convention earlier sketches used for ESP-NOW peer naming.
#define BLE_NAME_PREFIX  "SW-"
char myDeviceName[10];
uint8_t myMac[6];

#define MODE_GPS 0
#define PEER_STALE_TIMEOUT 5000

uint8_t currentMode = MODE_GPS;  // locked — BLE removed in this build

// ---- Haptic ----
#define HAPTIC_RANGE_M      10.0
#define HAPTIC_MIN_INTERVAL 50
#define HAPTIC_MAX_INTERVAL 800
#define HAPTIC_PULSE_MS     200

unsigned long lastHapticOn  = 0;
unsigned long lastHapticOff = 0;
bool hapticOn = false;

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

// ---- ESP-NOW Message ----
typedef struct __attribute__((packed)) {
  float lat;
  float lng;
  bool gpsValid;
  uint8_t satellites;
  uint8_t mode;
  int8_t txPower;
} PeerMessage;

// ---- Objects ----
Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);
TinyGPSPlus gps;

// ---- Peer / GPS state ----
PeerMessage peerMsg;
bool peerReceived = false;
unsigned long lastPeerTime = 0;
unsigned long lastSend = 0;
double gpsDistance = -1;

// ---- Display state ----
unsigned long lastDisplayUpdate = 0;
double prevDisplayDist = -999;
int prevBPM = -1;
bool prevFinger = false;
bool prevConnected = false;
bool forceRedraw = true;

const double EARTH_RADIUS = 6371000.0;

// ---- Target (loaded from NVS — no BLE write path in this build) ----
#define TARGET_NAME_MAX_LEN 24
bool     hasTarget                          = false;
uint8_t  targetMac[6]                       = {0};
char     targetName[TARGET_NAME_MAX_LEN + 1] = "";

// ---- NVS ----
Preferences prefs;
const char* PREFS_NAMESPACE = "sw-pair";
const char* PREFS_KEY_TMAC  = "tmac";
const char* PREFS_KEY_TNAME = "tname";

unsigned long buttonDownSince = 0;
bool buttonLatched = false;

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
#define COLOR_PINK   0xF81F

// =====================================================
//  MATH
// =====================================================

double haversine(double lat1, double lon1, double lat2, double lon2) {
  double dLat = radians(lat2 - lat1);
  double dLon = radians(lon2 - lon1);
  double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
             cos(radians(lat1)) * cos(radians(lat2)) *
             sin(dLon / 2.0) * sin(dLon / 2.0);
  double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
  return EARTH_RADIUS * c;
}

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
//  HAPTIC
// =====================================================

double getActiveDistance() {
  bool peerStale = (millis() - lastPeerTime > PEER_STALE_TIMEOUT);
  if (peerReceived && !peerStale && peerMsg.gpsValid && gps.location.isValid()) {
    return gpsDistance;
  }
  return -1;
}

void updateHaptic() {
  double dist = getActiveDistance();

  if (dist < 0 || dist > HAPTIC_RANGE_M) {
    if (hapticOn) {
      digitalWrite(MOTOR_PIN, LOW);
      hapticOn = false;
    }
    return;
  }

  float ratio = constrain(dist / HAPTIC_RANGE_M, 0.0, 1.0);
  unsigned long interval = HAPTIC_MIN_INTERVAL +
    (unsigned long)(ratio * (HAPTIC_MAX_INTERVAL - HAPTIC_MIN_INTERVAL));

  unsigned long now = millis();

  if (hapticOn) {
    if (now - lastHapticOn >= HAPTIC_PULSE_MS) {
      digitalWrite(MOTOR_PIN, LOW);
      hapticOn = false;
      lastHapticOff = now;
    }
  } else {
    if (now - lastHapticOff >= interval) {
      digitalWrite(MOTOR_PIN, HIGH);
      hapticOn = true;
      lastHapticOn = now;
    }
  }
}

// =====================================================
//  ESP-NOW CALLBACKS
// =====================================================

void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("[ESP-NOW] Send FAIL");
  }
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(PeerMessage)) return;
  if (!hasTarget) return;
  if (memcmp(info->src_addr, targetMac, 6) != 0) return;
  memcpy(&peerMsg, data, sizeof(PeerMessage));
  peerReceived = true;
  lastPeerTime = millis();
}

// =====================================================
//  NVS
// =====================================================

void loadTargetFromNvs() {
  prefs.begin(PREFS_NAMESPACE, true);
  size_t got = prefs.getBytesLength(PREFS_KEY_TMAC);
  if (got == 6) {
    prefs.getBytes(PREFS_KEY_TMAC, targetMac, 6);
    hasTarget = true;
    String nm = prefs.getString(PREFS_KEY_TNAME, "");
    strncpy(targetName, nm.c_str(), TARGET_NAME_MAX_LEN);
    targetName[TARGET_NAME_MAX_LEN] = '\0';
  } else {
    hasTarget = false;
    targetName[0] = '\0';
  }
  prefs.end();
  Serial.printf("[TARGET] Loaded: target=%s name=%s\n",
                hasTarget ? "yes" : "(none)", targetName);
}

void clearNvs() {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  hasTarget = false;
  memset(targetMac, 0, 6);
  targetName[0] = '\0';
  peerReceived = false;
  gpsDistance = -1;
  forceRedraw = true;
  Serial.println("[NVS] Cleared");
}

void updateButton() {
  int level = digitalRead(BUTTON_PIN);  // active-LOW
  unsigned long now = millis();
  if (level == LOW) {
    if (buttonDownSince == 0) {
      buttonDownSince = now;
    } else if (!buttonLatched && (now - buttonDownSince >= BUTTON_HOLD_MS)) {
      buttonLatched = true;
      Serial.println("[BTN] 5s hold — clearing NVS target");
      clearNvs();
    }
  } else {
    buttonDownSince = 0;
    buttonLatched = false;
  }
}

// =====================================================
//  DISPLAY
// =====================================================

uint16_t getProximityColor(double distance) {
  if (distance < 0) return COLOR_GRAY;
  if (distance < 1.0) return COLOR_GREEN;
  if (distance < 3.0) return COLOR_YELLOW;
  if (distance < 10.0) return COLOR_ORANGE;
  return COLOR_RED;
}

const char* getProximityLabel(double distance) {
  if (distance < 0) return "LOST";
  if (distance < 1.0) return "HERE";
  if (distance < 3.0) return "NEAR";
  if (distance < 10.0) return "CLOSE";
  if (distance < 50.0) return "FAR";
  return "DISTANT";
}

void drawCenteredText(const char* text, int y, uint16_t color, uint8_t size) {
  tft.setTextSize(size);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  tft.setTextColor(color, COLOR_BG);
  tft.setCursor((240 - w) / 2, y);
  tft.print(text);
}

void drawProximityRing(double distance, uint16_t color) {
  int cx = 120, cy = 120;
  int maxR = 55;
  int minR = 15;

  int r;
  if (distance < 0) {
    r = 0;
  } else if (distance < 0.5) {
    r = maxR;
  } else if (distance > 50) {
    r = minR;
  } else {
    r = maxR - (int)((distance / 50.0) * (maxR - minR));
  }

  tft.fillCircle(cx, cy, maxR + 2, COLOR_BG);

  if (r > 0) {
    tft.fillCircle(cx, cy, r, color);
    tft.drawCircle(cx, cy, maxR, COLOR_GRAY);
  } else {
    tft.drawCircle(cx, cy, maxR, COLOR_GRAY);
    tft.drawLine(cx - 12, cy - 12, cx + 12, cy + 12, COLOR_RED);
    tft.drawLine(cx - 12, cy + 12, cx + 12, cy - 12, COLOR_RED);
  }
}

void drawNotTrackingScreen() {
  tft.fillScreen(COLOR_BG);
  drawCenteredText("NOT", 80, COLOR_GRAY, 3);
  drawCenteredText("TRACKING", 115, COLOR_GRAY, 3);
  drawCenteredText("(BLE disabled)", 160, COLOR_GRAY, 1);
  drawCenteredText("Diagnostic 15", 175, COLOR_GRAY, 1);
}

void updateDisplay() {
  if (!hasTarget) {
    if (forceRedraw) {
      drawNotTrackingScreen();
      forceRedraw = false;
      prevDisplayDist = -999;
    }
    return;
  }

  double displayDist = getActiveDistance();
  bool connected = (displayDist >= 0);

  bool distChanged = forceRedraw ||
    (prevDisplayDist < 0 && displayDist >= 0) ||
    (prevDisplayDist >= 0 && displayDist < 0) ||
    (displayDist >= 0 && fabs(displayDist - prevDisplayDist) > 0.1) ||
    (currentBPM != prevBPM) ||
    (fingerDetected != prevFinger) ||
    (connected != prevConnected);

  if (!distChanged) return;

  prevDisplayDist = displayDist;
  prevBPM = currentBPM;
  prevFinger = fingerDetected;
  prevConnected = connected;
  forceRedraw = false;

  tft.fillScreen(COLOR_BG);

  tft.setTextSize(1);
  tft.setTextColor(COLOR_WHITE, COLOR_BG);
  tft.setCursor(10, 10);
  tft.print(targetName[0] ? targetName : "(target)");

  drawCenteredText("GPS", 8, COLOR_BLUE, 1);
  char satBuf[12];
  int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  snprintf(satBuf, sizeof(satBuf), "Sat:%d", sats);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_WHITE, COLOR_BG);
  tft.setCursor(190, 10);
  tft.print(satBuf);

  const char* status;
  uint16_t statusColor;
  if (connected) {
    status = "CONNECTED";
    statusColor = COLOR_GREEN;
  } else if (peerReceived) {
    status = "SEARCHING...";
    statusColor = COLOR_YELLOW;
  } else {
    status = "NO PEER";
    statusColor = COLOR_RED;
  }
  drawCenteredText(status, 24, statusColor, 1);

  const char* label = getProximityLabel(displayDist);
  uint16_t color = getProximityColor(displayDist);
  drawCenteredText(label, 38, color, 2);

  drawProximityRing(displayDist, color);

  char distBuf[20];
  if (displayDist < 0) {
    snprintf(distBuf, sizeof(distBuf), "---");
  } else if (displayDist < 100) {
    snprintf(distBuf, sizeof(distBuf), "%.1f m", displayDist);
  } else if (displayDist < 1000) {
    snprintf(distBuf, sizeof(distBuf), "%d m", (int)displayDist);
  } else {
    snprintf(distBuf, sizeof(distBuf), "%.2f km", displayDist / 1000.0);
  }
  drawCenteredText(distBuf, 182, COLOR_WHITE, 2);

  if (maxFound) {
    char hrBuf[20];
    if (fingerDetected && currentBPM > 0) {
      snprintf(hrBuf, sizeof(hrBuf), "%d BPM", currentBPM);
      drawCenteredText(hrBuf, 206, COLOR_RED, 2);
    } else if (fingerDetected) {
      drawCenteredText("Reading...", 210, COLOR_ORANGE, 1);
    } else {
      drawCenteredText("No finger", 210, COLOR_GRAY, 1);
    }
  }

  if (displayDist >= 0 && displayDist <= HAPTIC_RANGE_M) {
    tft.fillCircle(230, 230, 4, COLOR_PINK);
  }
}

// =====================================================
//  GPS DISTANCE
// =====================================================

void updateGpsDistance() {
  bool peerStale = (millis() - lastPeerTime > PEER_STALE_TIMEOUT);
  bool hasGpsFix = gps.location.isValid() && peerReceived && !peerStale && peerMsg.gpsValid;
  if (hasGpsFix) {
    gpsDistance = haversine(gps.location.lat(), gps.location.lng(),
                            peerMsg.lat, peerMsg.lng);
  } else {
    gpsDistance = -1;
  }
}

// =====================================================
//  SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("==========================================");
  Serial.println("  Spatial Wearable — BLE-DISABLED (15)");
  Serial.println("  Diagnostic: GPS + ESP-NOW + display only");
  Serial.println("==========================================");

  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  loadTargetFromNvs();

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

  // WiFi STA is required for ESP-NOW. BLE stack is intentionally NOT
  // initialized — that's the experiment.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(500);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  snprintf(myDeviceName, sizeof(myDeviceName), "%s%02X%02X",
           BLE_NAME_PREFIX, myMac[4], myMac[5]);
  Serial.printf("Device name: %s\n", myDeviceName);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: ESP-NOW init failed!");
    while (true) delay(1000);
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddr, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("ERROR: ESP-NOW peer add failed!");
    while (true) delay(1000);
  }
  Serial.println("[ESP-NOW] OK");

  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[GPS] UART OK");

  Serial.println("==========================================");
  Serial.println(" BLE DISABLED for this build.");
  Serial.println(" If GPS now acquires like 02_, BLE was the");
  Serial.println(" interference source.");
  Serial.println("==========================================\n");

  forceRedraw = true;
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

  if (millis() - lastSend >= 1000) {
    lastSend = millis();

    PeerMessage msg;
    msg.gpsValid = gps.location.isValid();
    msg.lat = msg.gpsValid ? gps.location.lat() : 0;
    msg.lng = msg.gpsValid ? gps.location.lng() : 0;
    msg.satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
    msg.mode = currentMode;
    msg.txPower = 9;

    esp_now_send(broadcastAddr, (uint8_t *)&msg, sizeof(msg));

    bool gpsFix = gps.location.isValid();
    int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
    Serial.printf("[STATUS] GPS:%.1fm | BPM:%d | Finger:%s\n",
      gpsDistance, currentBPM, fingerDetected ? "Y" : "N");
    Serial.printf("[GPS] Fix:%s | Lat:%.6f | Lng:%.6f | Sats:%d\n",
      gpsFix ? "YES" : "NO",
      gpsFix ? gps.location.lat() : 0.0,
      gpsFix ? gps.location.lng() : 0.0,
      sats);
  }

  updateButton();
  updateGpsDistance();

  if (millis() - lastDisplayUpdate >= 300) {
    lastDisplayUpdate = millis();
    updateDisplay();
  }
}
