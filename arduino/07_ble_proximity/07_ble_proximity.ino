// Hybrid GPS + BLE Proximity Tracker
// Flash the SAME sketch to both XIAO ESP32S3 boards.
// Auto-detects which board it's running on via MAC address.
//
// ============================================================
//  HOW IT WORKS
// ============================================================
//
//  Two modes, selected automatically based on GPS distance:
//
//  GPS MODE (>50m apart):
//    - Both boards exchange GPS coordinates via ESP-NOW
//    - Distance calculated with Haversine formula
//    - BLE radio is OFF to save power
//
//  BLE MODE (<=50m apart, or no GPS fix):
//    - Both boards advertise + scan via BLE simultaneously
//    - Distance estimated from RSSI (signal strength)
//    - GPS still runs in background for fallback
//    - ESP-NOW still active for GPS data exchange
//
//  Hysteresis: switch to BLE at <=50m, back to GPS at >=60m
//
//  WIRING (same on both boards):
//    GPS TXD  --> D7 (GPIO44)
//    GPS RXD  --> D6 (GPIO43)
//    GPS VCC  --> 3V3
//    GPS GND  --> GND
//    Display SCK  --> D8  (GPIO7)
//    Display MOSI --> D10 (GPIO9)
//    Display CS   --> D1  (GPIO2)
//    Display DC   --> D2  (GPIO3)
//    Display RST  --> D3  (GPIO4)
//    Display VCC  --> 3V3
//    Display GND  --> GND
//
//  Boards auto-discover each other via BLE service UUID.
//  No hardcoded MACs needed — just flash the same sketch to both.
//
// ============================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <TinyGPSPlus.h>
#include <NimBLEDevice.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <math.h>

// ---- Pin Definitions ----

// GPS UART (matches CLAUDE.md standard wiring)
#define GPS_RX_PIN 44  // D7 - GPIO44 - connects to GPS TXD
#define GPS_TX_PIN 43  // D6 - GPIO43 - connects to GPS RXD

// Display SPI
#define TFT_SCK   7   // D8 - GPIO7
#define TFT_MOSI  9   // D10 - GPIO9
#define TFT_CS    2   // D1 - GPIO2
#define TFT_DC    3   // D2 - GPIO3
#define TFT_RST   4   // D3 - GPIO4

// ---- ESP-NOW ----
uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---- BLE Configuration ----
#define BLE_SERVICE_UUID    "12345678-1234-5678-1234-56781234abcd"
#define BLE_NAME_PREFIX     "SW-"   // Each board advertises as "SW-XXXX" (last 2 MAC bytes)
char myBleName[10];                 // e.g. "SW-09B0"

// RSSI calibration
#define MEASURED_POWER  -55   // RSSI at 1 meter (calibrate empirically)
#define PATH_LOSS_N     2.5   // Path loss exponent (2=open air, 3-4=indoors)

// RSSI smoothing
#define RSSI_BUFFER_SIZE 10
int rssiBuffer[RSSI_BUFFER_SIZE];
int rssiBufferIdx = 0;
int rssiBufferCount = 0;

// ---- Mode Switching ----
#define MODE_GPS 0
#define MODE_BLE 1
#define SWITCH_TO_BLE_M   50.0   // Switch to BLE when GPS distance <= 50m
#define SWITCH_TO_GPS_M   60.0   // Switch back to GPS when GPS distance >= 60m
#define BLE_LOST_TIMEOUT  5000   // ms before declaring BLE lost
#define PEER_STALE_TIMEOUT 5000  // ms before declaring peer data stale

uint8_t currentMode = MODE_BLE;  // Start in BLE mode (works without GPS)
bool bleActive = false;

// ---- ESP-NOW Message ----
typedef struct __attribute__((packed)) {
  float lat;
  float lng;
  bool gpsValid;
  uint8_t satellites;
  uint8_t mode;       // Current tracking mode
  int8_t txPower;     // BLE TX power level
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
bool forceRedraw = true;

// Earth radius in meters
const double EARTH_RADIUS = 6371000.0;

// ---- NimBLE scan callback ----
NimBLEScan* pBLEScan = nullptr;
NimBLEAdvertising* pAdvertising = nullptr;

// ---- Haversine formula ----
double haversine(double lat1, double lon1, double lat2, double lon2) {
  double dLat = radians(lat2 - lat1);
  double dLon = radians(lon2 - lon1);
  double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
             cos(radians(lat1)) * cos(radians(lat2)) *
             sin(dLon / 2.0) * sin(dLon / 2.0);
  double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
  return EARTH_RADIUS * c;
}

// ---- RSSI to Distance ----
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

  // Calculate mean
  long sum = 0;
  for (int i = 0; i < rssiBufferCount; i++) sum += rssiBuffer[i];
  int mean = sum / rssiBufferCount;

  // Calculate std dev for outlier rejection
  long sqSum = 0;
  for (int i = 0; i < rssiBufferCount; i++) {
    int diff = rssiBuffer[i] - mean;
    sqSum += (long)diff * diff;
  }
  double stdDev = sqrt((double)sqSum / rssiBufferCount);

  // Average excluding outliers (>2 std devs)
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

// ---- ESP-NOW Callbacks ----
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  // Silent unless error
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

// ---- BLE Scan Callback ----
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    // Match any device with our service UUID that isn't us
    if (!device->isAdvertisingService(NimBLEUUID(BLE_SERVICE_UUID))) return;

    const char* name = device->haveName() ? device->getName().c_str() : "?";

    // Skip our own advertisements
    if (strcmp(name, myBleName) == 0) return;

    int rssi = device->getRSSI();
    addRssiSample(rssi);
    lastRssi = rssi;
    lastBleTime = millis();

    // Remember peer name on first discovery
    if (!peerBleFound) {
      strncpy(peerBleName, name, sizeof(peerBleName) - 1);
      Serial.printf("[BLE] *** PEER DISCOVERED: %s ***\n", peerBleName);
    }
    peerBleFound = true;

    int smoothed = getSmoothedRssi();
    bleDistance = rssiToDistance(smoothed);

    Serial.printf("[BLE] Peer '%s' RSSI: %d (smoothed: %d) -> %.1fm\n",
                  peerBleName, rssi, smoothed, bleDistance);
  }
};

static ScanCallbacks scanCallbacks;

// ---- BLE Start/Stop ----
void startBLE() {
  if (bleActive) return;

  Serial.println("[BLE] Starting...");

  NimBLEDevice::init(myBleName);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // Max TX power for range

  // Start advertising
  NimBLEServer* pServer = NimBLEDevice::createServer();
  NimBLEService* pService = pServer->createService(BLE_SERVICE_UUID);
  pService->start();

  pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
  pAdvertising->setName(myBleName);
  pAdvertising->enableScanResponse(true);
  pAdvertising->setMinInterval(160);  // 100ms (units of 0.625ms)
  pAdvertising->setMaxInterval(320);  // 200ms
  pAdvertising->start();

  // Start scanning
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(&scanCallbacks, false);
  pBLEScan->setInterval(200);
  pBLEScan->setWindow(100);
  pBLEScan->setActiveScan(true);
  pBLEScan->setDuplicateFilter(false);  // We want every advertisement for RSSI updates
  pBLEScan->setMaxResults(0);           // Don't store results, use callbacks
  pBLEScan->start(0, false, true);   // Continuous, non-blocking

  bleActive = true;
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
  bleDistance = -1;
  rssiBufferCount = 0;
  rssiBufferIdx = 0;
  Serial.println("[BLE] Stopped");
}

// ---- Display Functions ----

// Color definitions
#define COLOR_BG      0x0000  // Black
#define COLOR_GREEN   0x07E0
#define COLOR_YELLOW  0xFFE0
#define COLOR_ORANGE  0xFD20
#define COLOR_RED     0xF800
#define COLOR_GRAY    0x7BEF
#define COLOR_CYAN    0x07FF
#define COLOR_WHITE   0xFFFF
#define COLOR_BLUE    0x001F

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
  int cx = 120, cy = 130;
  int maxR = 80;
  int minR = 20;

  // Closer = larger filled ring. Map 0-50m to maxR-minR
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

  // Clear ring area
  tft.fillCircle(cx, cy, maxR + 2, COLOR_BG);

  if (r > 0) {
    // Filled proximity circle
    tft.fillCircle(cx, cy, r, color);

    // Outer ring outline
    tft.drawCircle(cx, cy, maxR, COLOR_GRAY);
  } else {
    // Lost state - just outline
    tft.drawCircle(cx, cy, maxR, COLOR_GRAY);
    tft.drawLine(cx - 15, cy - 15, cx + 15, cy + 15, COLOR_RED);
    tft.drawLine(cx - 15, cy + 15, cx + 15, cy - 15, COLOR_RED);
  }
}

void drawSignalBars(int rssi) {
  int x = 185, y = 25;
  int barW = 6, barGap = 3;
  int maxBars = 5;

  // Map RSSI to bars: -90 or worse=0, -30 or better=5
  int bars = 0;
  if (rssi > -40) bars = 5;
  else if (rssi > -50) bars = 4;
  else if (rssi > -60) bars = 3;
  else if (rssi > -70) bars = 2;
  else if (rssi > -85) bars = 1;

  // Clear area
  tft.fillRect(x - 2, y - 2, maxBars * (barW + barGap) + 4, 28, COLOR_BG);

  for (int i = 0; i < maxBars; i++) {
    int barH = 6 + i * 4;  // Progressively taller
    int bx = x + i * (barW + barGap);
    int by = y + 24 - barH;
    if (i < bars) {
      tft.fillRect(bx, by, barW, barH, COLOR_GREEN);
    } else {
      tft.drawRect(bx, by, barW, barH, COLOR_GRAY);
    }
  }
}

void updateDisplay() {
  double displayDist;
  bool peerStale = (millis() - lastPeerTime > PEER_STALE_TIMEOUT);
  bool bleLost = bleActive && (millis() - lastBleTime > BLE_LOST_TIMEOUT);

  if (currentMode == MODE_BLE && peerBleFound && !bleLost) {
    displayDist = bleDistance;
  } else if (peerReceived && !peerStale && peerMsg.gpsValid && gps.location.isValid()) {
    displayDist = gpsDistance;
  } else {
    displayDist = -1;  // No distance available
  }

  // Only redraw if changed significantly or forced
  bool distChanged = forceRedraw ||
    (prevDisplayDist < 0 && displayDist >= 0) ||
    (prevDisplayDist >= 0 && displayDist < 0) ||
    (displayDist >= 0 && fabs(displayDist - prevDisplayDist) > 0.1) ||
    (currentMode != prevDisplayMode);

  if (!distChanged) return;

  prevDisplayDist = displayDist;
  prevDisplayMode = currentMode;
  forceRedraw = false;

  // Clear screen
  tft.fillScreen(COLOR_BG);

  // Mode indicator (top)
  if (currentMode == MODE_BLE) {
    drawCenteredText("BLE", 8, COLOR_CYAN, 2);
    if (bleActive && lastRssi != 0) {
      drawSignalBars(lastRssi);
    }
  } else {
    drawCenteredText("GPS", 8, COLOR_BLUE, 2);
    // Satellite count
    char satBuf[12];
    int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
    snprintf(satBuf, sizeof(satBuf), "Sat:%d", sats);
    tft.setTextSize(1);
    tft.setTextColor(COLOR_WHITE, COLOR_BG);
    tft.setCursor(190, 12);
    tft.print(satBuf);
  }

  // Board name
  tft.setTextSize(1);
  tft.setTextColor(COLOR_GRAY, COLOR_BG);
  tft.setCursor(10, 12);
  tft.print(myBleName);

  // Proximity label
  const char* label = getProximityLabel(displayDist);
  uint16_t color = getProximityColor(displayDist);
  drawCenteredText(label, 35, color, 2);

  // Proximity ring
  drawProximityRing(displayDist, color);

  // Distance text (below ring)
  char distBuf[20];
  if (displayDist < 0) {
    snprintf(distBuf, sizeof(distBuf), "---");
  } else if (displayDist < 1.0) {
    snprintf(distBuf, sizeof(distBuf), "%.1f m", displayDist);
  } else if (displayDist < 100) {
    snprintf(distBuf, sizeof(distBuf), "%.1f m", displayDist);
  } else if (displayDist < 1000) {
    snprintf(distBuf, sizeof(distBuf), "%d m", (int)displayDist);
  } else {
    snprintf(distBuf, sizeof(distBuf), "%.2f km", displayDist / 1000.0);
  }
  drawCenteredText(distBuf, 218, COLOR_WHITE, 2);

  // Peer status (bottom)
  const char* status;
  if (displayDist >= 0) {
    status = "Connected";
  } else if (peerReceived) {
    status = "Searching...";
  } else {
    status = "No peer";
  }

  tft.setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(status, 0, 0, &x1, &y1, &w, &h);
  tft.setTextColor(displayDist >= 0 ? COLOR_GREEN : COLOR_YELLOW, COLOR_BG);
  tft.setCursor((240 - w) / 2, 55);
  tft.print(status);
}

// ---- Mode Switching Logic ----
void updateMode() {
  bool peerStale = (millis() - lastPeerTime > PEER_STALE_TIMEOUT);
  bool bleLost = bleActive && !peerBleFound && (millis() - lastBleTime > BLE_LOST_TIMEOUT);
  bool hasGpsFix = gps.location.isValid() && peerReceived && !peerStale && peerMsg.gpsValid;

  if (hasGpsFix) {
    gpsDistance = haversine(gps.location.lat(), gps.location.lng(),
                           peerMsg.lat, peerMsg.lng);
  }

  switch (currentMode) {
    case MODE_GPS:
      // Switch to BLE when close enough
      if (hasGpsFix && gpsDistance <= SWITCH_TO_BLE_M) {
        Serial.printf("[MODE] GPS -> BLE (distance: %.1fm <= %.0fm)\n",
                      gpsDistance, SWITCH_TO_BLE_M);
        currentMode = MODE_BLE;
        startBLE();
        forceRedraw = true;
      }
      break;

    case MODE_BLE:
      // Switch to GPS when far enough (only if GPS available)
      if (hasGpsFix && gpsDistance >= SWITCH_TO_GPS_M) {
        Serial.printf("[MODE] BLE -> GPS (distance: %.1fm >= %.0fm)\n",
                      gpsDistance, SWITCH_TO_GPS_M);
        currentMode = MODE_GPS;
        stopBLE();
        forceRedraw = true;
      }
      // Also fall back to GPS if BLE is lost and GPS available
      if (bleLost && hasGpsFix && gpsDistance > SWITCH_TO_BLE_M) {
        Serial.println("[MODE] BLE -> GPS (BLE lost, GPS available)");
        currentMode = MODE_GPS;
        stopBLE();
        forceRedraw = true;
      }
      break;
  }
}

// ---- Setup ----
void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("==========================================");
  Serial.println("  Hybrid GPS + BLE Proximity Tracker");
  Serial.println("==========================================");

  // ---- Display init ----
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(COLOR_BG);
  drawCenteredText("Initializing...", 110, COLOR_YELLOW, 2);

  // ---- WiFi/ESP-NOW init ----
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(500);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  // Generate unique BLE name from MAC address
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  snprintf(myBleName, sizeof(myBleName), "%s%02X%02X", BLE_NAME_PREFIX, myMac[4], myMac[5]);
  Serial.printf("My MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
    myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
  Serial.printf("BLE name: %s (auto-generated)\n", myBleName);

  // ESP-NOW init
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
    Serial.println("ERROR: Failed to add ESP-NOW peer!");
    while (true) delay(1000);
  }

  Serial.println("ESP-NOW initialized");

  // ---- GPS init ----
  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("GPS UART initialized");

  // ---- Start in BLE mode (works without GPS fix) ----
  startBLE();

  Serial.println("==========================================");
  Serial.printf("Mode: BLE (default, awaiting GPS fix)\n");
  Serial.printf("BLE name: %s | Scanning for service UUID peers\n", myBleName);
  Serial.printf("Thresholds: BLE<=%.0fm | GPS>=%.0fm\n",
                SWITCH_TO_BLE_M, SWITCH_TO_GPS_M);
  Serial.println("==========================================\n");

  forceRedraw = true;
}

// ---- Main Loop ----
void loop() {
  // Feed GPS parser
  while (Serial1.available()) {
    gps.encode(Serial1.read());
  }

  // Send GPS position via ESP-NOW every 1 second
  if (millis() - lastSend >= 1000) {
    lastSend = millis();

    PeerMessage msg;
    msg.gpsValid = gps.location.isValid();
    msg.lat = msg.gpsValid ? gps.location.lat() : 0;
    msg.lng = msg.gpsValid ? gps.location.lng() : 0;
    msg.satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
    msg.mode = currentMode;
    msg.txPower = 9;  // ESP_PWR_LVL_P9

    esp_now_send(broadcastAddr, (uint8_t *)&msg, sizeof(msg));

    // Serial status
    bool bleLost = bleActive && peerBleFound && (millis() - lastBleTime > BLE_LOST_TIMEOUT);
    Serial.printf("[STATUS] Mode: %s | ", currentMode == MODE_BLE ? "BLE" : "GPS");

    // BLE peer status
    if (bleActive) {
      if (peerBleFound && !bleLost) {
        Serial.printf("Peer: %s (%.1fm, RSSI:%d) | ", peerBleName, bleDistance, lastRssi);
      } else if (peerBleFound && bleLost) {
        Serial.printf("Peer: %s (LOST %lus ago) | ", peerBleName, (millis() - lastBleTime) / 1000);
      } else {
        Serial.printf("Peer: scanning... | ");
      }
    }

    // GPS status
    if (gps.location.isValid()) {
      Serial.printf("GPS: %.6f,%.6f (sats:%d) | ",
        gps.location.lat(), gps.location.lng(),
        gps.satellites.isValid() ? gps.satellites.value() : 0);
    } else {
      Serial.print("GPS: no fix | ");
    }
    if (gpsDistance >= 0) {
      Serial.printf("GPS dist: %.1fm", gpsDistance);
    }
    Serial.println();
  }

  // Update mode switching
  updateMode();

  // Update display every 300ms
  if (millis() - lastDisplayUpdate >= 300) {
    lastDisplayUpdate = millis();
    updateDisplay();
  }
}
