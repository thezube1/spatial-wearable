// Full Spatial Wearable — GPS + BLE Proximity + Heart Rate + Haptic Feedback
// Flash the SAME sketch to both XIAO ESP32S3 boards.
//
// ============================================================
//  COMPONENTS
// ============================================================
//  - XIAO ESP32S3 (main MCU)
//  - GC9A01 1.28" Round TFT (SPI)
//  - ATGM336H GPS (UART)
//  - MAX30102 PPG Sensor (I2C) — heart rate
//  - Vibration Motor (GPIO)
//
//  WIRING (same on both boards):
//    GPS TXD  --> D7 (GPIO44)
//    GPS RXD  --> D6 (GPIO43)
//    Display SCK  --> D8  (GPIO7)
//    Display MOSI --> D10 (GPIO9)
//    Display CS   --> D1  (GPIO2)
//    Display DC   --> D3  (GPIO4)
//    Display RST  --> D2  (GPIO3)
//    MAX30102 SDA --> D4  (GPIO5)
//    MAX30102 SCL --> D5  (GPIO6)
//    Motor IN     --> D0  (GPIO1)
//
//  BEHAVIOR:
//    - GPS + BLE both always on
//    - BLE used for distance when close (<2m), GPS when far (>2m)
//    - Display shows: distance, HR (BPM), pairing status
//    - Haptic: within 10m, motor buzzes; closer = faster buzz
//
// ============================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <TinyGPSPlus.h>
#include <NimBLEDevice.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include "MAX30105.h"
#include <math.h>

// ---- Pin Definitions ----
#define GPS_RX_PIN 44  // D7 — connects to GPS TXD
#define GPS_TX_PIN 43  // D6 — connects to GPS RXD

#define TFT_SCK   7    // D8
#define TFT_MOSI  9    // D10
#define TFT_CS    2    // D1
#define TFT_DC    4    // D3
#define TFT_RST   3    // D2

#define MOTOR_PIN 1    // D0

// ---- ESP-NOW ----
uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---- BLE Configuration ----
#define BLE_SERVICE_UUID    "12345678-1234-5678-1234-56781234abcd"
#define BLE_NAME_PREFIX     "SW-"
char myBleName[10];

// RSSI calibration
#define MEASURED_POWER  -55
#define PATH_LOSS_N     2.5

// RSSI smoothing
#define RSSI_BUFFER_SIZE 10
int rssiBuffer[RSSI_BUFFER_SIZE];
int rssiBufferIdx = 0;
int rssiBufferCount = 0;

// ---- Mode Switching ----
#define MODE_GPS 0
#define MODE_BLE 1
#define SWITCH_TO_BLE_M   5.0    // Use BLE when GPS says <= 5m
#define SWITCH_TO_GPS_M   2.0    // Use GPS when BLE says >= 2m
#define BLE_LOST_TIMEOUT  10000  // 10s before declaring peer lost (was 5s)
#define PEER_STALE_TIMEOUT 5000

// BLE retry / resilience
#define BLE_SCAN_RESTART_INTERVAL 8000   // Restart scan every 8s to prevent stalls
#define BLE_FULL_RESTART_TIMEOUT  30000  // Full BLE reinit if peer lost for 30s

uint8_t currentMode = MODE_BLE;
bool bleActive = false;
unsigned long lastScanRestart = 0;
unsigned long peerLostSince = 0;       // When we first noticed peer was gone
bool peerWasFound = false;             // Track if we ever had a peer

// ---- Haptic Configuration ----
#define HAPTIC_RANGE_M     10.0   // Start buzzing within this range
#define HAPTIC_MIN_INTERVAL 50    // ms — fastest buzz (very close)
#define HAPTIC_MAX_INTERVAL 800   // ms — slowest buzz (at 10m)
#define HAPTIC_PULSE_MS     200   // Duration of each buzz pulse (was 60)

unsigned long lastHapticOn = 0;
unsigned long lastHapticOff = 0;
bool hapticOn = false;

// ---- Heart Rate (MAX30102) ----
MAX30105 particleSensor;
bool maxFound = false;

// Simple beat detection via IR delta
#define HR_SAMPLE_INTERVAL  20    // ms between IR reads
#define HR_BUFFER_SIZE      75    // ~1.5s of samples at 20ms
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

// ---- State ----
char peerBleName[10] = "";

// GPS state
PeerMessage peerMsg;
bool peerReceived = false;
unsigned long lastPeerTime = 0;
unsigned long lastSend = 0;
double gpsDistance = -1;

// BLE state
int lastRssi = 0;
double bleDistance = -1;
unsigned long lastBleTime = 0;
bool peerBleFound = false;

// Display state
unsigned long lastDisplayUpdate = 0;
double prevDisplayDist = -999;
uint8_t prevDisplayMode = 255;
int prevBPM = -1;
bool prevFinger = false;
bool prevConnected = false;
bool forceRedraw = true;

// Earth radius
const double EARTH_RADIUS = 6371000.0;

// NimBLE
NimBLEScan* pBLEScan = nullptr;
NimBLEAdvertising* pAdvertising = nullptr;

// ---- Colors ----
#define COLOR_BG      0x0000
#define COLOR_GREEN   0x07E0
#define COLOR_YELLOW  0xFFE0
#define COLOR_ORANGE  0xFD20
#define COLOR_RED     0xF800
#define COLOR_GRAY    0x7BEF
#define COLOR_CYAN    0x07FF
#define COLOR_WHITE   0xFFFF
#define COLOR_BLUE    0x001F
#define COLOR_PINK    0xF81F

// =====================================================
//  MATH / DISTANCE HELPERS
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

double rssiToDistance(int rssi) {
  if (rssi == 0) return -1;
  double ratio = (MEASURED_POWER - rssi) / (10.0 * PATH_LOSS_N);
  return pow(10.0, ratio);
}

void addRssiSample(int rssi) {
  rssiBuffer[rssiBufferIdx] = rssi;
  rssiBufferIdx = (rssiBufferIdx + 1) % RSSI_BUFFER_SIZE;
  if (rssiBufferCount < RSSI_BUFFER_SIZE) rssiBufferCount++;
}

int getSmoothedRssi() {
  if (rssiBufferCount == 0) return 0;
  long sum = 0;
  for (int i = 0; i < rssiBufferCount; i++) sum += rssiBuffer[i];
  int mean = sum / rssiBufferCount;

  long sqSum = 0;
  for (int i = 0; i < rssiBufferCount; i++) {
    int diff = rssiBuffer[i] - mean;
    sqSum += (long)diff * diff;
  }
  double stdDev = sqrt((double)sqSum / rssiBufferCount);

  long filteredSum = 0;
  int filteredCount = 0;
  for (int i = 0; i < rssiBufferCount; i++) {
    if (abs(rssiBuffer[i] - mean) <= 2 * stdDev) {
      filteredSum += rssiBuffer[i];
      filteredCount++;
    }
  }
  return filteredCount > 0 ? filteredSum / filteredCount : mean;
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

  // Simple peak detection: count zero-crossings of derivative
  // above a noise threshold to estimate beats
  int count = HR_BUFFER_SIZE;

  // Compute mean for baseline
  long irSum = 0;
  for (int i = 0; i < count; i++) irSum += irBuffer[i];
  long irMean = irSum / count;

  // Count rising-edge zero crossings of (sample - mean)
  int beats = 0;
  bool wasAbove = false;
  for (int i = 0; i < count; i++) {
    bool above = irBuffer[i] > irMean;
    if (above && !wasAbove) beats++;
    wasAbove = above;
  }

  // Convert to BPM: buffer covers (count * HR_SAMPLE_INTERVAL) ms
  float durationSec = (count * HR_SAMPLE_INTERVAL) / 1000.0;
  int bpm = (int)(beats / durationSec * 60.0);

  if (bpm >= HR_MIN_BPM && bpm <= HR_MAX_BPM) {
    currentBPM = bpm;
  }
  // else keep previous reading
}

// =====================================================
//  HAPTIC FEEDBACK
// =====================================================

double getActiveDistance() {
  bool peerStale = (millis() - lastPeerTime > PEER_STALE_TIMEOUT);
  bool bleLost = bleActive && peerBleFound && (millis() - lastBleTime > BLE_LOST_TIMEOUT);

  if (currentMode == MODE_BLE && peerBleFound && !bleLost) {
    return bleDistance;
  } else if (peerReceived && !peerStale && peerMsg.gpsValid && gps.location.isValid()) {
    return gpsDistance;
  }
  return -1;
}

void updateHaptic() {
  double dist = getActiveDistance();

  // No buzz if out of range or no distance
  if (dist < 0 || dist > HAPTIC_RANGE_M) {
    if (hapticOn) {
      digitalWrite(MOTOR_PIN, LOW);
      hapticOn = false;
    }
    return;
  }

  // Map distance to buzz interval: 0m -> HAPTIC_MIN_INTERVAL, 10m -> HAPTIC_MAX_INTERVAL
  float ratio = constrain(dist / HAPTIC_RANGE_M, 0.0, 1.0);
  unsigned long interval = HAPTIC_MIN_INTERVAL + (unsigned long)(ratio * (HAPTIC_MAX_INTERVAL - HAPTIC_MIN_INTERVAL));

  unsigned long now = millis();

  if (hapticOn) {
    // Turn off after pulse duration
    if (now - lastHapticOn >= HAPTIC_PULSE_MS) {
      digitalWrite(MOTOR_PIN, LOW);
      hapticOn = false;
      lastHapticOff = now;
    }
  } else {
    // Turn on after interval
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
  if (len == sizeof(PeerMessage)) {
    memcpy(&peerMsg, data, sizeof(PeerMessage));
    peerReceived = true;
    lastPeerTime = millis();
  }
}

// =====================================================
//  BLE SCAN CALLBACK
// =====================================================

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    if (!device->isAdvertisingService(NimBLEUUID(BLE_SERVICE_UUID))) return;
    const char* name = device->haveName() ? device->getName().c_str() : "?";
    if (strcmp(name, myBleName) == 0) return;

    int rssi = device->getRSSI();
    addRssiSample(rssi);
    lastRssi = rssi;
    lastBleTime = millis();

    if (!peerBleFound) {
      strncpy(peerBleName, name, sizeof(peerBleName) - 1);
      Serial.printf("[BLE] *** PEER DISCOVERED: %s ***\n", peerBleName);
    }
    peerBleFound = true;
    peerWasFound = true;
    peerLostSince = 0;  // Reset lost timer — peer is alive

    int smoothed = getSmoothedRssi();
    bleDistance = rssiToDistance(smoothed);
  }
};

static ScanCallbacks scanCallbacks;

// =====================================================
//  BLE START / STOP
// =====================================================

void startBLE() {
  if (bleActive) return;
  Serial.println("[BLE] Starting...");

  NimBLEDevice::init(myBleName);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  NimBLEServer* pServer = NimBLEDevice::createServer();
  NimBLEService* pService = pServer->createService(BLE_SERVICE_UUID);
  pService->start();

  pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
  pAdvertising->setName(myBleName);
  pAdvertising->enableScanResponse(true);
  pAdvertising->setMinInterval(160);
  pAdvertising->setMaxInterval(320);
  pAdvertising->start();

  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(&scanCallbacks, false);
  pBLEScan->setInterval(200);
  pBLEScan->setWindow(100);
  pBLEScan->setActiveScan(true);
  pBLEScan->setDuplicateFilter(false);
  pBLEScan->setMaxResults(0);
  pBLEScan->start(0, false, true);

  bleActive = true;
  lastScanRestart = millis();
  Serial.println("[BLE] Advertising + Scanning active");
}

void stopBLE() {
  if (!bleActive) return;
  Serial.println("[BLE] Stopping...");

  if (pBLEScan) pBLEScan->stop();
  if (pAdvertising) pAdvertising->stop();
  NimBLEDevice::deinit(true);

  bleActive = false;
  peerBleFound = false;
  peerWasFound = false;
  peerLostSince = 0;
  bleDistance = -1;
  rssiBufferCount = 0;
  rssiBufferIdx = 0;
  Serial.println("[BLE] Stopped");
}

// Restart just the scan — cheap, fixes stalled scans
void restartBleScan() {
  if (!bleActive || !pBLEScan) return;

  pBLEScan->stop();
  delay(50);
  pBLEScan->start(0, false, true);
  lastScanRestart = millis();
  Serial.println("[BLE] Scan restarted");
}

// Full teardown + reinit — fixes deeper NimBLE issues
void restartBLEFull() {
  Serial.println("[BLE] === FULL RESTART ===");
  stopBLE();
  delay(200);
  startBLE();
  peerLostSince = 0;
}

// Called from loop() — handles periodic scan restarts and full recovery
void updateBleRetry() {
  if (!bleActive) return;

  unsigned long now = millis();
  bool bleLost = peerWasFound && (now - lastBleTime > BLE_LOST_TIMEOUT);

  // Periodic scan restart to prevent NimBLE scan stalls
  if (now - lastScanRestart >= BLE_SCAN_RESTART_INTERVAL) {
    restartBleScan();
  }

  // Track how long peer has been lost
  if (bleLost) {
    if (peerLostSince == 0) {
      peerLostSince = now;
      Serial.printf("[BLE] Peer lost — will full-restart in %ds\n",
                    BLE_FULL_RESTART_TIMEOUT / 1000);
    }

    // Full BLE restart if lost for too long
    if (now - peerLostSince >= BLE_FULL_RESTART_TIMEOUT) {
      restartBLEFull();
    }
  } else {
    peerLostSince = 0;
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

void drawSignalBars(int rssi) {
  int x = 185, y = 8;
  int barW = 5, barGap = 2;
  int maxBars = 5;

  int bars = 0;
  if (rssi > -40) bars = 5;
  else if (rssi > -50) bars = 4;
  else if (rssi > -60) bars = 3;
  else if (rssi > -70) bars = 2;
  else if (rssi > -85) bars = 1;

  for (int i = 0; i < maxBars; i++) {
    int barH = 4 + i * 3;
    int bx = x + i * (barW + barGap);
    int by = y + 18 - barH;
    if (i < bars) {
      tft.fillRect(bx, by, barW, barH, COLOR_GREEN);
    } else {
      tft.drawRect(bx, by, barW, barH, COLOR_GRAY);
    }
  }
}

void updateDisplay() {
  double displayDist = getActiveDistance();

  bool connected = (displayDist >= 0);

  // Check if anything changed
  bool distChanged = forceRedraw ||
    (prevDisplayDist < 0 && displayDist >= 0) ||
    (prevDisplayDist >= 0 && displayDist < 0) ||
    (displayDist >= 0 && fabs(displayDist - prevDisplayDist) > 0.1) ||
    (currentMode != prevDisplayMode) ||
    (currentBPM != prevBPM) ||
    (fingerDetected != prevFinger) ||
    (connected != prevConnected);

  if (!distChanged) return;

  prevDisplayDist = displayDist;
  prevDisplayMode = currentMode;
  prevBPM = currentBPM;
  prevFinger = fingerDetected;
  prevConnected = connected;
  forceRedraw = false;

  tft.fillScreen(COLOR_BG);

  // ---- Top row: mode + board name + signal ----
  tft.setTextSize(1);
  tft.setTextColor(COLOR_GRAY, COLOR_BG);
  tft.setCursor(10, 10);
  tft.print(myBleName);

  if (currentMode == MODE_BLE) {
    drawCenteredText("BLE", 8, COLOR_CYAN, 1);
    if (bleActive && lastRssi != 0) drawSignalBars(lastRssi);
  } else {
    drawCenteredText("GPS", 8, COLOR_BLUE, 1);
    char satBuf[12];
    int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
    snprintf(satBuf, sizeof(satBuf), "Sat:%d", sats);
    tft.setTextSize(1);
    tft.setTextColor(COLOR_WHITE, COLOR_BG);
    tft.setCursor(190, 10);
    tft.print(satBuf);
  }

  // ---- Pairing status ----
  const char* status;
  uint16_t statusColor;
  if (connected) {
    status = "CONNECTED";
    statusColor = COLOR_GREEN;
  } else if (peerReceived || peerBleFound) {
    status = "SEARCHING...";
    statusColor = COLOR_YELLOW;
  } else {
    status = "NO PEER";
    statusColor = COLOR_RED;
  }
  drawCenteredText(status, 24, statusColor, 1);

  // ---- Proximity label ----
  const char* label = getProximityLabel(displayDist);
  uint16_t color = getProximityColor(displayDist);
  drawCenteredText(label, 38, color, 2);

  // ---- Proximity ring (centered) ----
  drawProximityRing(displayDist, color);

  // ---- Distance text (below ring) ----
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

  // ---- Heart Rate (bottom area) ----
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

  // ---- Haptic indicator ----
  if (displayDist >= 0 && displayDist <= HAPTIC_RANGE_M) {
    tft.fillCircle(230, 230, 4, COLOR_PINK);  // small dot = haptic active
  }
}

// =====================================================
//  MODE SWITCHING
// =====================================================

void updateMode() {
  bool peerStale = (millis() - lastPeerTime > PEER_STALE_TIMEOUT);
  bool hasGpsFix = gps.location.isValid() && peerReceived && !peerStale && peerMsg.gpsValid;
  bool bleLost = peerWasFound && (millis() - lastBleTime > BLE_LOST_TIMEOUT);
  bool hasBle = peerBleFound && !bleLost && bleDistance >= 0;

  // Always update GPS distance when available
  if (hasGpsFix) {
    gpsDistance = haversine(gps.location.lat(), gps.location.lng(),
                           peerMsg.lat, peerMsg.lng);
  }

  uint8_t prevMode = currentMode;

  switch (currentMode) {
    case MODE_GPS:
      // Switch to BLE when GPS says we're close enough
      if (hasGpsFix && gpsDistance <= SWITCH_TO_BLE_M && hasBle) {
        currentMode = MODE_BLE;
      }
      // BLE says close (<5m) but GPS disagrees — trust BLE at close range
      else if (hasBle && bleDistance < 5.0 && hasGpsFix && gpsDistance > bleDistance) {
        currentMode = MODE_BLE;
      }
      // Also switch to BLE if GPS is unavailable but BLE is working
      else if (!hasGpsFix && hasBle) {
        currentMode = MODE_BLE;
      }
      break;

    case MODE_BLE:
      // Switch to GPS when BLE says we're far enough
      if (hasBle && bleDistance >= SWITCH_TO_GPS_M) {
        if (hasGpsFix) {
          currentMode = MODE_GPS;
        }
        // If no GPS fix, stay on BLE (it's all we have)
      }
      // Fallback to GPS if BLE is lost
      if (currentMode == MODE_BLE && !hasBle && hasGpsFix) {
        currentMode = MODE_GPS;
      }
      break;
  }

  if (currentMode != prevMode) {
    Serial.printf("[MODE] %s -> %s (BLE:%.1fm GPS:%.1fm)\n",
      prevMode == MODE_BLE ? "BLE" : "GPS",
      currentMode == MODE_BLE ? "BLE" : "GPS",
      bleDistance, gpsDistance);
    forceRedraw = true;
  }
}

// =====================================================
//  SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("==========================================");
  Serial.println("  Spatial Wearable — Full System");
  Serial.println("==========================================");

  // ---- Motor ----
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  // ---- I2C + MAX30102 ----
  Wire.begin(5, 6);
  Serial.print("[MAX30102] ");
  if (particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    particleSensor.setup(0x1F, 4, 2, 100, 411, 4096);  // low power config
    maxFound = true;
    Serial.println("OK");
  } else {
    Serial.println("NOT FOUND — HR disabled");
  }

  // ---- Display ----
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(COLOR_BG);
  drawCenteredText("Starting...", 110, COLOR_YELLOW, 2);

  // ---- WiFi / ESP-NOW ----
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(500);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  snprintf(myBleName, sizeof(myBleName), "%s%02X%02X", BLE_NAME_PREFIX, myMac[4], myMac[5]);
  Serial.printf("BLE name: %s\n", myBleName);

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

  // ---- GPS ----
  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[GPS] UART OK");

  // ---- Start BLE ----
  startBLE();

  Serial.println("==========================================");
  Serial.println("Both GPS + BLE always on");
  Serial.printf("Use BLE when GPS<=%.0fm | Use GPS when BLE>=%.0fm | Haptic<=%.0fm\n",
                SWITCH_TO_BLE_M, SWITCH_TO_GPS_M, HAPTIC_RANGE_M);
  Serial.println("==========================================\n");

  forceRedraw = true;
}

// =====================================================
//  LOOP
// =====================================================

void loop() {
  // Feed GPS
  while (Serial1.available()) {
    gps.encode(Serial1.read());
  }

  // Sample heart rate (non-blocking, runs every HR_SAMPLE_INTERVAL ms)
  sampleHeartRate();

  // Haptic feedback (non-blocking)
  updateHaptic();

  // Send GPS via ESP-NOW every 1s
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

    // Serial debug
    bool gpsFix = gps.location.isValid();
    Serial.printf("[STATUS] Mode:%s | Dist:%.1fm | BLE:%.1fm | GPS:%.1fm | BPM:%d | Finger:%s\n",
      currentMode == MODE_BLE ? "BLE" : "GPS",
      getActiveDistance(),
      bleDistance,
      gpsDistance,
      currentBPM,
      fingerDetected ? "Y" : "N");
    Serial.printf("[GPS] Fix:%s | Lat:%.6f | Lng:%.6f | Sats:%d\n",
      gpsFix ? "YES" : "NO",
      gpsFix ? gps.location.lat() : 0.0,
      gpsFix ? gps.location.lng() : 0.0,
      gps.satellites.isValid() ? gps.satellites.value() : 0);
  }

  // BLE retry / scan health
  updateBleRetry();

  // Mode switching
  updateMode();

  // Display update every 300ms
  if (millis() - lastDisplayUpdate >= 300) {
    lastDisplayUpdate = millis();
    updateDisplay();
  }
}
