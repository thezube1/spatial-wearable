// Full Spatial Wearable — ESP-NOW DISABLED diagnostic build (16)
// Fork of 14_wearable_peer_location. This fork is the *complement* of 15:
// it keeps all BLE functionality and instead removes ESP-NOW entirely, to
// isolate whether the periodic 1 Hz ESP-NOW broadcasts are what's
// desensitizing the GPS receiver.
//
// REMOVED vs 14:
//   - <esp_now.h> include
//   - broadcastAddr + esp_now_init / register_*_cb / add_peer
//   - onDataSent / onDataRecv callbacks
//   - The 1 Hz esp_now_send() call from loop()
//   - PeerMessage construction inside loop() (struct kept for BLE forwarding)
//
// KEPT vs 14:
//   - All NimBLE: advertising, scanning, server, characteristics, callbacks
//   - WiFi STA mode (left up so radio state matches 14 minus ESP-NOW only —
//     BLE itself doesn't need WiFi, but isolating one variable is the point)
//   - GPS UART, MAX30102, GC9A01 display, motor, NVS pairing + target
//   - Pairing-mode UI, owner auth, target-select via BLE write
//
// Without ESP-NOW the wearable can never receive a peer's GPS, so:
//   - peerReceived stays false; gpsDistance stays -1
//   - mode switcher will keep us in MODE_BLE (only data source available)
//   - BLE peer-location notify (abd3) always emits valid=0
//
// Test plan: flash 16 in the same setup where 02 acquires GPS. If GPS now
// locks like 02, ESP-NOW transmissions were the blocker. If 15 (no BLE)
// also fixed it, then BOTH radios contribute and we'll need both gone.
//
// ----- original header (14) below -----
// Full Spatial Wearable — Peer Location build (14)
// Adds peer-location BLE notify (…abd3) so the iOS map can render the tracked
// peer alongside the wearable's own location.
//
// ----- original header (13) below -----
// Full Spatial Wearable — Target Select build (13)
// Fork of 12_wearable_location_ble. Adds per-group target tracking:
//   - New BLE characteristic (…abd2) that the iOS app writes with the MAC and
//     display name of the group member this wearable should lock onto.
//     Payload: [6-byte MAC][1-byte name_len][name UTF-8...]. A 1-byte {0x00}
//     write clears the target (solo group → "Not tracking" state).
//   - Target persisted in NVS (sw-pair namespace, keys target_mac/target_name)
//     so it survives reboots; cleared when the owner is cleared.
//   - ESP-NOW receive callback filters by sender MAC — only packets from the
//     selected target are accepted.
//   - BLE scan callback filters by the expected "SW-XXXX" name derived from
//     target MAC last two bytes, so RSSI distance locks to one peer.
//   - Display shows target name where device label used to sit, and renders a
//     dedicated "Not tracking" state when no target is set.
//
// ----- original header (10) below -----
// Full Spatial Wearable — Persistent Pairing build (10)
// Flash the SAME sketch to both XIAO ESP32S3 boards.
//
// Derived from 09_wearable_pairing.ino. Adds:
//   - NVS-backed pairing state (Preferences namespace "sw-pair") so the
//     wearable remembers which Supabase user_id owns it across reboots.
//   - GPIO0 (BOOT button) 5-second hold to forget the owner and enter a
//     dedicated pairing mode. Fresh devices also start in pairing mode.
//   - Owner-write characteristic (…abcf) accepted only in pairing mode:
//     iOS writes the Supabase user_id, ESP stores it, exits pairing mode.
//   - Owner-auth characteristic (…abd0) required on every reconnect:
//     iOS writes the same user_id; wrong or missing token => disconnect.
//   - Dedicated "PAIRING MODE" full-screen UI on the GC9A01 display.
//   - MAC characteristic read is gated when paired: only readable while
//     in pairing mode or after the central has authed on the current link.
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
//    - Exposes read-only MAC characteristic for iOS pairing
//
// ============================================================

#include <WiFi.h>
#include <esp_wifi.h>
#include <TinyGPSPlus.h>
#include <NimBLEDevice.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include "MAX30105.h"
#include <Preferences.h>
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

// BOOT button on XIAO ESP32S3 (strapping pin — safe as INPUT_PULLUP after boot).
#define PAIR_BUTTON_PIN   0
#define PAIR_HOLD_MS      5000UL
#define PAIR_MODE_TIMEOUT 300000UL   // 5 min auto-exit if nothing writes the owner
#define AUTH_GRACE_MS     3000UL     // central must write owner-auth within 3s

// ---- ESP-NOW (DISABLED in this build — see header) ----

// ---- BLE Configuration ----
#define BLE_SERVICE_UUID         "12345678-1234-5678-1234-56781234abcd"
#define BLE_MAC_CHAR_UUID        "12345678-1234-5678-1234-56781234abce"
#define BLE_OWNER_WRITE_CHAR_UUID "12345678-1234-5678-1234-56781234abcf"
#define BLE_OWNER_AUTH_CHAR_UUID  "12345678-1234-5678-1234-56781234abd0"
#define BLE_LOCATION_CHAR_UUID    "12345678-1234-5678-1234-56781234abd1"
#define BLE_TARGET_CHAR_UUID      "12345678-1234-5678-1234-56781234abd2"
#define BLE_PEER_LOCATION_CHAR_UUID "12345678-1234-5678-1234-56781234abd3"
#define TARGET_NAME_MAX_LEN       24
#define LOCATION_NOTIFY_INTERVAL_MS 15000UL
#define PEER_LOCATION_NOTIFY_INTERVAL_MS 3000UL
#define BLE_NAME_PREFIX          "SW-"
char myBleName[10];
uint8_t myMac[6];
char myMacStr[18];  // "AA:BB:CC:DD:EE:FF" + null terminator

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
bool prevPhoneLinked = false;
bool forceRedraw = true;

// Earth radius
const double EARTH_RADIUS = 6371000.0;

// NimBLE
NimBLEScan* pBLEScan = nullptr;
NimBLEAdvertising* pAdvertising = nullptr;
NimBLEServer* pBleServer = nullptr;
NimBLEService* pBleService = nullptr;
NimBLECharacteristic* pMacCharacteristic = nullptr;
NimBLECharacteristic* pOwnerWriteCharacteristic = nullptr;
NimBLECharacteristic* pOwnerAuthCharacteristic = nullptr;
NimBLECharacteristic* pLocationCharacteristic = nullptr;
NimBLECharacteristic* pTargetCharacteristic = nullptr;
NimBLECharacteristic* pPeerLocationCharacteristic = nullptr;
unsigned long lastLocationNotify = 0;
unsigned long lastPeerLocationNotify = 0;

// ---- Target selection state ----
bool     hasTarget         = false;
uint8_t  targetMac[6]      = {0};
char     targetName[TARGET_NAME_MAX_LEN + 1] = "";
char     targetBleName[10] = "";   // expected "SW-XXXX" for scan filtering

// ---- Persistent pairing state ----
Preferences prefs;
const char* PREFS_NAMESPACE = "sw-pair";
const char* PREFS_KEY_PAIRED = "paired";
const char* PREFS_KEY_OWNER  = "owner";
const char* PREFS_KEY_TMAC   = "tmac";
const char* PREFS_KEY_TNAME  = "tname";

bool     isPaired        = false;
String   ownerId         = "";          // Supabase user_id (UUID, 36 chars)
bool     pairingMode     = true;        // fresh boot => pairing until NVS says otherwise
unsigned long pairingEnteredAt = 0;
unsigned long buttonDownSince  = 0;
bool     buttonLatched   = false;       // avoid re-triggering while still held

// Per-connection auth: central must write owner-auth within AUTH_GRACE_MS.
bool     authedConn       = false;
unsigned long connectedAt = 0;
uint16_t currentConnHandle = 0xFFFF;
bool     hasActiveConn    = false;

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

  if (dist < 0 || dist > HAPTIC_RANGE_M) {
    if (hapticOn) {
      digitalWrite(MOTOR_PIN, LOW);
      hapticOn = false;
    }
    return;
  }

  float ratio = constrain(dist / HAPTIC_RANGE_M, 0.0, 1.0);
  unsigned long interval = HAPTIC_MIN_INTERVAL + (unsigned long)(ratio * (HAPTIC_MAX_INTERVAL - HAPTIC_MIN_INTERVAL));

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
//  BLE SCAN CALLBACK
// =====================================================

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    if (!device->isAdvertisingService(NimBLEUUID(BLE_SERVICE_UUID))) return;
    const char* name = device->haveName() ? device->getName().c_str() : "?";
    if (strcmp(name, myBleName) == 0) return;
    // Only lock onto the currently-selected target. Without a target, don't
    // accumulate RSSI from strangers.
    if (!hasTarget) return;
    if (strcmp(name, targetBleName) != 0) return;

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
    peerLostSince = 0;

    int smoothed = getSmoothedRssi();
    bleDistance = rssiToDistance(smoothed);
  }
};

static ScanCallbacks scanCallbacks;

// =====================================================
//  PERSISTENT PAIRING (NVS)
// =====================================================

void recomputeTargetBleName() {
  if (hasTarget) {
    snprintf(targetBleName, sizeof(targetBleName), "%s%02X%02X",
             BLE_NAME_PREFIX, targetMac[4], targetMac[5]);
  } else {
    targetBleName[0] = '\0';
  }
}

void loadPairing() {
  prefs.begin(PREFS_NAMESPACE, true);  // read-only
  isPaired = prefs.getBool(PREFS_KEY_PAIRED, false);
  ownerId  = prefs.getString(PREFS_KEY_OWNER, "");
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
  recomputeTargetBleName();
  Serial.printf("[PAIR] Loaded: paired=%d owner=%s target=%s name=%s\n",
                isPaired, ownerId.c_str(),
                hasTarget ? targetBleName : "(none)", targetName);
}

void saveTarget(const uint8_t mac[6], const char* name) {
  memcpy(targetMac, mac, 6);
  hasTarget = true;
  strncpy(targetName, name ? name : "", TARGET_NAME_MAX_LEN);
  targetName[TARGET_NAME_MAX_LEN] = '\0';
  recomputeTargetBleName();
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putBytes(PREFS_KEY_TMAC, targetMac, 6);
  prefs.putString(PREFS_KEY_TNAME, targetName);
  prefs.end();
  // Reset peer state so we don't hang onto stale readings from the previous target.
  peerBleFound = false;
  peerWasFound = false;
  peerReceived = false;
  bleDistance = -1;
  gpsDistance = -1;
  rssiBufferCount = 0;
  rssiBufferIdx = 0;
  peerBleName[0] = '\0';
  lastPeerLocationNotify = 0;  // force a peer-location notify next loop
  forceRedraw = true;
  Serial.printf("[TARGET] Saved %s (%02X:%02X:%02X:%02X:%02X:%02X)\n",
                targetName, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void clearTarget() {
  hasTarget = false;
  memset(targetMac, 0, 6);
  targetName[0] = '\0';
  targetBleName[0] = '\0';
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.remove(PREFS_KEY_TMAC);
  prefs.remove(PREFS_KEY_TNAME);
  prefs.end();
  peerBleFound = false;
  peerWasFound = false;
  peerReceived = false;
  bleDistance = -1;
  gpsDistance = -1;
  rssiBufferCount = 0;
  rssiBufferIdx = 0;
  peerBleName[0] = '\0';
  lastPeerLocationNotify = 0;  // force a peer-location notify (valid=0) next loop
  forceRedraw = true;
  Serial.println("[TARGET] Cleared");
}

void savePairing(const String& uid) {
  prefs.begin(PREFS_NAMESPACE, false); // rw
  prefs.putBool(PREFS_KEY_PAIRED, true);
  prefs.putString(PREFS_KEY_OWNER, uid);
  prefs.end();
  isPaired = true;
  ownerId  = uid;
  Serial.printf("[PAIR] Saved owner=%s\n", uid.c_str());
}

void clearPairing() {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  isPaired = false;
  ownerId  = "";
  hasTarget = false;
  memset(targetMac, 0, 6);
  targetName[0] = '\0';
  targetBleName[0] = '\0';
  Serial.println("[PAIR] Cleared (pairing + target)");
}

void enterPairingMode(const char* reason) {
  Serial.printf("[PAIR] === ENTER PAIRING MODE (%s) ===\n", reason);
  clearPairing();
  pairingMode = true;
  pairingEnteredAt = millis();
  authedConn = false;
  forceRedraw = true;
  // Disconnect any currently-connected central so the new pairing starts clean.
  if (pBleServer && hasActiveConn) {
    pBleServer->disconnect(currentConnHandle);
  }
}

void exitPairingMode(const String& uid) {
  savePairing(uid);
  pairingMode = false;
  authedConn = true;                  // the connection that just wrote the owner is trusted
  forceRedraw = true;
  Serial.println("[PAIR] === EXIT PAIRING MODE (linked) ===");
}

// =====================================================
//  BLE CALLBACKS (server + characteristic writes)
// =====================================================

class SWServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    currentConnHandle = info.getConnHandle();
    hasActiveConn = true;
    authedConn = false;
    connectedAt = millis();
    Serial.printf("[BLE] Central connected (handle=%u)\n", currentConnHandle);
    // Keep advertising so the wearable remains discoverable if disconnected.
    NimBLEDevice::startAdvertising();
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& info, int reason) override {
    Serial.printf("[BLE] Central disconnected (reason=%d)\n", reason);
    hasActiveConn = false;
    authedConn = false;
    currentConnHandle = 0xFFFF;
    NimBLEDevice::startAdvertising();
  }
};

class OwnerWriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    String v = String(c->getValue().c_str());
    v.trim();
    Serial.printf("[BLE] owner-write recv (%u bytes): %s\n", v.length(), v.c_str());
    if (!pairingMode) {
      Serial.println("[BLE] owner-write rejected: not in pairing mode");
      return;
    }
    if (v.length() < 8 || v.length() > 64) {
      Serial.println("[BLE] owner-write rejected: bad length");
      return;
    }
    exitPairingMode(v);
  }
};

class OwnerAuthCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    String v = String(c->getValue().c_str());
    v.trim();
    Serial.printf("[BLE] owner-auth recv: %s\n", v.c_str());
    if (!isPaired) {
      // Not paired yet — the normal path is owner-write during pairing mode.
      Serial.println("[BLE] owner-auth ignored: device not paired");
      return;
    }
    if (v == ownerId) {
      authedConn = true;
      Serial.println("[BLE] owner-auth OK");
    } else {
      Serial.println("[BLE] owner-auth FAIL — disconnecting");
      authedConn = false;
      if (pBleServer && hasActiveConn) pBleServer->disconnect(info.getConnHandle());
    }
  }
};

class MacReadCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    // Pairing mode: anyone may read MAC (that's how iOS links the device).
    // Paired + unauthed: hide the MAC so random centrals can't scrape it.
    if (pairingMode || authedConn) {
      c->setValue((uint8_t*)myMacStr, 17);
    } else {
      c->setValue((uint8_t*)"", 0);
    }
  }
};

// Target-select: iOS writes [6-byte MAC][1-byte name_len][name...]. A 1-byte
// {0x00} write clears the target. Only accepted on an authed connection so
// random centrals can't steer the wearable.
class TargetWriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    if (!isPaired || !authedConn) {
      Serial.println("[BLE] target-write rejected: not authed");
      return;
    }
    std::string v = c->getValue();
    if (v.size() == 1 && (uint8_t)v[0] == 0x00) {
      clearTarget();
      return;
    }
    if (v.size() < 7) {
      Serial.printf("[BLE] target-write rejected: short (%u)\n", (unsigned)v.size());
      return;
    }
    uint8_t mac[6];
    memcpy(mac, v.data(), 6);
    uint8_t nameLen = (uint8_t)v[6];
    if (nameLen > TARGET_NAME_MAX_LEN || (size_t)(7 + nameLen) > v.size()) {
      Serial.println("[BLE] target-write rejected: bad name length");
      return;
    }
    char nm[TARGET_NAME_MAX_LEN + 1] = {0};
    memcpy(nm, v.data() + 7, nameLen);
    nm[nameLen] = '\0';
    saveTarget(mac, nm);
  }
};

static SWServerCallbacks   serverCallbacks;
static OwnerWriteCallbacks ownerWriteCallbacks;
static OwnerAuthCallbacks  ownerAuthCallbacks;
static MacReadCallbacks    macReadCallbacks;
static TargetWriteCallbacks targetWriteCallbacks;

// =====================================================
//  BUTTON — 5s hold to re-pair
// =====================================================

void updatePairButton() {
  int level = digitalRead(PAIR_BUTTON_PIN);  // active-LOW
  unsigned long now = millis();
  if (level == LOW) {
    if (buttonDownSince == 0) {
      buttonDownSince = now;
    } else if (!buttonLatched && (now - buttonDownSince >= PAIR_HOLD_MS)) {
      buttonLatched = true;
      enterPairingMode("button-hold");
    }
  } else {
    buttonDownSince = 0;
    buttonLatched = false;
  }
}

void updateConnAuthTimeout() {
  if (!hasActiveConn) return;
  if (pairingMode || authedConn) return;
  if (!isPaired) return;  // nothing to auth against
  if (millis() - connectedAt < AUTH_GRACE_MS) return;
  Serial.println("[BLE] auth grace expired — disconnecting unauthed central");
  if (pBleServer) pBleServer->disconnect(currentConnHandle);
}

// =====================================================
//  BLE START / STOP
// =====================================================

// Push the current GPS location to subscribed centrals. Throttled to
// LOCATION_NOTIFY_INTERVAL_MS for power efficiency. Always notifies (even
// without a fix) so the phone UI can show a "searching" state.
void notifyLocationIfDue() {
  if (!pLocationCharacteristic) return;
  unsigned long now = millis();
  if (lastLocationNotify != 0 && now - lastLocationNotify < LOCATION_NOTIFY_INTERVAL_MS) return;
  lastLocationNotify = now;

  uint8_t buf[9];
  bool valid = gps.location.isValid();
  float lat = valid ? (float)gps.location.lat() : 0.0f;
  float lon = valid ? (float)gps.location.lng() : 0.0f;
  buf[0] = valid ? 1 : 0;
  memcpy(&buf[1], &lat, 4);
  memcpy(&buf[5], &lon, 4);
  pLocationCharacteristic->setValue(buf, sizeof(buf));
  pLocationCharacteristic->notify();
  Serial.printf("[BLE] Location notify: valid=%d lat=%.6f lon=%.6f\n", buf[0], lat, lon);
}

// Push the currently-tracked peer's GPS to subscribed centrals so the phone
// can render the target alongside the owner's wearable on the map. Payload
// format matches abd1: [valid(u8), lat(float32 LE), lon(float32 LE)].
// valid=0 when there is no target, the target's packets are stale, or the
// target has no GPS fix.
void notifyPeerLocationIfDue() {
  if (!pPeerLocationCharacteristic) return;
  unsigned long now = millis();
  if (lastPeerLocationNotify != 0 && now - lastPeerLocationNotify < PEER_LOCATION_NOTIFY_INTERVAL_MS) return;
  lastPeerLocationNotify = now;

  bool peerStale = (now - lastPeerTime > PEER_STALE_TIMEOUT);
  bool valid = hasTarget && peerReceived && !peerStale && peerMsg.gpsValid;
  float lat = valid ? peerMsg.lat : 0.0f;
  float lon = valid ? peerMsg.lng : 0.0f;

  uint8_t buf[9];
  buf[0] = valid ? 1 : 0;
  memcpy(&buf[1], &lat, 4);
  memcpy(&buf[5], &lon, 4);
  pPeerLocationCharacteristic->setValue(buf, sizeof(buf));
  pPeerLocationCharacteristic->notify();
  Serial.printf("[BLE] Peer-location notify: valid=%d lat=%.6f lon=%.6f\n", buf[0], lat, lon);
}

void startBLE() {
  if (bleActive) return;
  Serial.println("[BLE] Starting...");

  NimBLEDevice::init(myBleName);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  pBleServer = NimBLEDevice::createServer();
  pBleServer->setCallbacks(&serverCallbacks);
  pBleService = pBleServer->createService(BLE_SERVICE_UUID);

  // Read-only MAC characteristic. Value is populated on-demand in the read
  // callback so we can gate it (only readable in pairing mode or after auth).
  pMacCharacteristic = pBleService->createCharacteristic(
    BLE_MAC_CHAR_UUID,
    NIMBLE_PROPERTY::READ
  );
  pMacCharacteristic->setCallbacks(&macReadCallbacks);
  pMacCharacteristic->setValue((uint8_t*)myMacStr, 17);
  Serial.printf("[BLE] MAC characteristic value: %s\n", myMacStr);

  // Owner-write: iOS writes the Supabase user_id once during pairing mode.
  pOwnerWriteCharacteristic = pBleService->createCharacteristic(
    BLE_OWNER_WRITE_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE
  );
  pOwnerWriteCharacteristic->setCallbacks(&ownerWriteCallbacks);

  // Owner-auth: iOS writes the user_id on every reconnect to prove identity.
  pOwnerAuthCharacteristic = pBleService->createCharacteristic(
    BLE_OWNER_AUTH_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE
  );
  pOwnerAuthCharacteristic->setCallbacks(&ownerAuthCallbacks);

  // Location: NOTIFY-only characteristic the phone subscribes to for the
  // wearable's GPS coordinates. Payload = [valid(u8), lat(float32 LE), lon(float32 LE)].
  pLocationCharacteristic = pBleService->createCharacteristic(
    BLE_LOCATION_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  // Target-select: iOS picks which group member this wearable locks onto.
  pTargetCharacteristic = pBleService->createCharacteristic(
    BLE_TARGET_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE
  );
  pTargetCharacteristic->setCallbacks(&targetWriteCallbacks);

  // Peer location: NOTIFY-only characteristic carrying the tracked target's
  // GPS (forwarded from ESP-NOW). Same 9-byte payload as abd1.
  pPeerLocationCharacteristic = pBleService->createCharacteristic(
    BLE_PEER_LOCATION_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  pBleService->start();

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

  pBleServer = nullptr;
  pBleService = nullptr;
  pMacCharacteristic = nullptr;
  pOwnerWriteCharacteristic = nullptr;
  pOwnerAuthCharacteristic = nullptr;
  pLocationCharacteristic = nullptr;
  pTargetCharacteristic = nullptr;
  pPeerLocationCharacteristic = nullptr;
  pBLEScan = nullptr;
  pAdvertising = nullptr;

  bleActive = false;
  peerBleFound = false;
  peerWasFound = false;
  peerLostSince = 0;
  bleDistance = -1;
  rssiBufferCount = 0;
  rssiBufferIdx = 0;
  Serial.println("[BLE] Stopped");
}

void restartBleScan() {
  if (!bleActive || !pBLEScan) return;

  pBLEScan->stop();
  delay(50);
  pBLEScan->start(0, false, true);
  lastScanRestart = millis();
  Serial.println("[BLE] Scan restarted");
}

void restartBLEFull() {
  Serial.println("[BLE] === FULL RESTART ===");
  stopBLE();
  delay(200);
  startBLE();
  peerLostSince = 0;
}

void updateBleRetry() {
  if (!bleActive) return;

  unsigned long now = millis();
  bool bleLost = peerWasFound && (now - lastBleTime > BLE_LOST_TIMEOUT);

  if (now - lastScanRestart >= BLE_SCAN_RESTART_INTERVAL) {
    restartBleScan();
  }

  if (bleLost) {
    if (peerLostSince == 0) {
      peerLostSince = now;
      Serial.printf("[BLE] Peer lost — will full-restart in %ds\n",
                    BLE_FULL_RESTART_TIMEOUT / 1000);
    }

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

void drawPairingScreen() {
  tft.fillScreen(COLOR_BG);
  drawCenteredText("PAIRING", 70, COLOR_CYAN, 2);
  drawCenteredText("MODE", 95, COLOR_CYAN, 2);
  drawCenteredText(myBleName, 130, COLOR_WHITE, 2);
  drawCenteredText("Open the app", 165, COLOR_GRAY, 1);

  // Pulsing ring for visual feedback.
  int cx = 120, cy = 205;
  unsigned long t = millis();
  int r = 6 + (int)((sin(t / 300.0) + 1.0) * 5.0);  // 6..16
  tft.drawCircle(cx, cy, r, COLOR_CYAN);
}

// Tiny phone icon in the top-right. Blue filled when an iOS central is
// connected and has passed owner-auth; grey outline otherwise.
void drawPhoneIcon(bool linked) {
  int x = 215, y = 6;     // top-right, offset from edge to clear the round mask
  int w = 12, h = 20;
  uint16_t fg  = linked ? COLOR_CYAN : COLOR_GRAY;
  uint16_t bg  = COLOR_BG;
  // Clear the region so the icon doesn't smear on state changes.
  tft.fillRect(x - 1, y - 1, w + 2, h + 2, bg);
  // Phone body
  tft.drawRoundRect(x, y, w, h, 2, fg);
  if (linked) {
    tft.fillRoundRect(x + 2, y + 2, w - 4, h - 6, 1, fg);
  }
  // Home dot
  tft.fillCircle(x + w / 2, y + h - 3, 1, fg);
}

void drawNotTrackingScreen() {
  tft.fillScreen(COLOR_BG);
  drawCenteredText("NOT", 80, COLOR_GRAY, 3);
  drawCenteredText("TRACKING", 115, COLOR_GRAY, 3);
  drawCenteredText("Pick a group member", 160, COLOR_GRAY, 1);
  drawCenteredText("in the app", 175, COLOR_GRAY, 1);
  drawPhoneIcon(hasActiveConn && authedConn);
}

void updateDisplay() {
  if (pairingMode) {
    if (forceRedraw) {
      drawPairingScreen();
      forceRedraw = false;
      prevDisplayDist = -999;
      prevDisplayMode = 255;
    } else {
      // Keep the pulsing ring alive without full redraw.
      int cx = 120, cy = 205;
      tft.fillCircle(cx, cy, 18, COLOR_BG);
      unsigned long t = millis();
      int r = 6 + (int)((sin(t / 300.0) + 1.0) * 5.0);
      tft.drawCircle(cx, cy, r, COLOR_CYAN);
    }
    return;
  }

  if (!hasTarget) {
    bool phoneLinked = hasActiveConn && authedConn;
    if (forceRedraw) {
      drawNotTrackingScreen();
      forceRedraw = false;
      prevDisplayDist = -999;
      prevDisplayMode = 255;
      prevPhoneLinked = phoneLinked;
    } else if (phoneLinked != prevPhoneLinked) {
      drawPhoneIcon(phoneLinked);
      prevPhoneLinked = phoneLinked;
    }
    return;
  }

  double displayDist = getActiveDistance();

  bool connected = (displayDist >= 0);
  bool phoneLinked = hasActiveConn && authedConn;

  // Fast path: if only the phone-link state changed, redraw just the icon
  // instead of the whole screen so the main UI stays flicker-free.
  bool distChanged = forceRedraw ||
    (prevDisplayDist < 0 && displayDist >= 0) ||
    (prevDisplayDist >= 0 && displayDist < 0) ||
    (displayDist >= 0 && fabs(displayDist - prevDisplayDist) > 0.1) ||
    (currentMode != prevDisplayMode) ||
    (currentBPM != prevBPM) ||
    (fingerDetected != prevFinger) ||
    (connected != prevConnected);

  if (!distChanged) {
    if (phoneLinked != prevPhoneLinked) {
      drawPhoneIcon(phoneLinked);
      prevPhoneLinked = phoneLinked;
    }
    return;
  }

  prevDisplayDist = displayDist;
  prevDisplayMode = currentMode;
  prevBPM = currentBPM;
  prevFinger = fingerDetected;
  prevConnected = connected;
  prevPhoneLinked = phoneLinked;
  forceRedraw = false;

  tft.fillScreen(COLOR_BG);

  // Target name (who we're locking onto) in the slot that used to show the
  // wearable's own BLE name.
  tft.setTextSize(1);
  tft.setTextColor(COLOR_WHITE, COLOR_BG);
  tft.setCursor(10, 10);
  tft.print(targetName[0] ? targetName : targetBleName);

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

  drawPhoneIcon(phoneLinked);
}

// =====================================================
//  MODE SWITCHING
// =====================================================

void updateMode() {
  bool peerStale = (millis() - lastPeerTime > PEER_STALE_TIMEOUT);
  bool hasGpsFix = gps.location.isValid() && peerReceived && !peerStale && peerMsg.gpsValid;
  bool bleLost = peerWasFound && (millis() - lastBleTime > BLE_LOST_TIMEOUT);
  bool hasBle = peerBleFound && !bleLost && bleDistance >= 0;

  if (hasGpsFix) {
    gpsDistance = haversine(gps.location.lat(), gps.location.lng(),
                           peerMsg.lat, peerMsg.lng);
  }

  uint8_t prevMode = currentMode;

  switch (currentMode) {
    case MODE_GPS:
      if (hasGpsFix && gpsDistance <= SWITCH_TO_BLE_M && hasBle) {
        currentMode = MODE_BLE;
      }
      else if (hasBle && bleDistance < 5.0 && hasGpsFix && gpsDistance > bleDistance) {
        currentMode = MODE_BLE;
      }
      else if (!hasGpsFix && hasBle) {
        currentMode = MODE_BLE;
      }
      break;

    case MODE_BLE:
      if (hasBle && bleDistance >= SWITCH_TO_GPS_M) {
        if (hasGpsFix) {
          currentMode = MODE_GPS;
        }
      }
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
  Serial.println("  Spatial Wearable — Phone Indicator (11)");
  Serial.println("==========================================");

  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  pinMode(PAIR_BUTTON_PIN, INPUT_PULLUP);

  // Load pairing state from NVS. If never paired, start in pairing mode.
  loadPairing();
  pairingMode = !isPaired;
  pairingEnteredAt = millis();

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

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(500);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  snprintf(myBleName, sizeof(myBleName), "%s%02X%02X", BLE_NAME_PREFIX, myMac[4], myMac[5]);
  // Format the MAC into the canonical 17-byte ASCII string exposed over BLE.
  snprintf(myMacStr, sizeof(myMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
  Serial.printf("BLE name: %s\n", myBleName);
  Serial.printf("MAC:      %s\n", myMacStr);

  Serial.println("[ESP-NOW] DISABLED for this build");

  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[GPS] UART OK");

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
  while (Serial1.available()) {
    gps.encode(Serial1.read());
  }

  sampleHeartRate();
  updateHaptic();

  if (millis() - lastSend >= 1000) {
    lastSend = millis();

    // ESP-NOW broadcast removed in this build — kept the 1 Hz status print
    // so logs still tick on the same cadence as 14_ for easy comparison.

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

  updatePairButton();
  updateConnAuthTimeout();

  // Auto-exit pairing mode after a long idle to avoid leaving the device
  // advertising as un-owned forever.
  if (pairingMode && isPaired && millis() - pairingEnteredAt > PAIR_MODE_TIMEOUT) {
    pairingMode = false;
    forceRedraw = true;
  }

  updateBleRetry();
  updateMode();
  notifyLocationIfDue();
  notifyPeerLocationIfDue();

  if (millis() - lastDisplayUpdate >= 300) {
    lastDisplayUpdate = millis();
    updateDisplay();
  }
}
