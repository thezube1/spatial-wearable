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
//    UI button (tactile) --> D9 (GPIO8) + GND (active LOW). Do NOT use D10/GPIO9 — that pad is SPI MOSI to the LCD.
//
//  BEHAVIOR (firmware 16 — group-targets):
//    - GPS + BLE both always on
//    - BLE used for distance when close (<2m), GPS when far (>2m)
//    - Display shows: distance, HR (BPM), pairing status
//    - Haptic: within 10m, motor buzzes; closer = faster buzz
//    - Exposes read-only MAC characteristic for iOS pairing
//    - iOS pushes a candidate list of up to 8 group members (…abd4)
//    - On NAV: short tap enters target-select; quick taps cycle; hold 3s
//      confirms; idle 8s cancels. Selected target mirrored back to iOS via
//      the …abd5 notify characteristic.
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
#include <Fonts/FreeMono9pt7b.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <Preferences.h>
#include <string.h>
#include <math.h>

// ---- Pin Definitions ----
// XIAO ESP32S3: Arduino/ESP core uses SoC GPIO numbers, not the "D" index.
//   D0..D5  -> GPIO1..6   |  D6,D7 -> GPIO43,44 (UART)  |  D8,D9,D10 -> GPIO7,8,9
//   Onboard BOOT is GPIO0 only (not the same as header D0 = GPIO1).
#define GPS_RX_PIN 44  // D7 — connects to GPS TXD
#define GPS_TX_PIN 43  // D6 — connects to GPS RXD

#define TFT_SCK   7    // D8
#define TFT_MOSI  9    // D10
#define TFT_CS    2    // D1
#define TFT_DC    4    // D3
#define TFT_RST   3    // D2

#define MOTOR_PIN 1    // D0

// UI: external tactile — one leg D9 (GPIO8), other leg GND (INPUT_PULLUP, pressed = LOW).
// D10/GPIO9 is TFT MOSI; reading a switch there always looks "released" while SPI owns the pin.
#define PAIR_BUTTON_PIN   8
#if PAIR_BUTTON_PIN == TFT_MOSI
#error "PAIR_BUTTON_PIN must not equal TFT_MOSI (D10) — wire the switch to D9 (GPIO8) instead."
#endif
#define PAIR_MODE_TIMEOUT 300000UL   // 5 min auto-exit if nothing writes the owner
#define AUTH_GRACE_MS     3000UL     // central must write owner-auth within 3s

// ---- UI button (D9 / GPIO8, tactile to GND) ----
// - HOME: release after 0.5~1s -> NAV; release after 1~4s -> BLE pairing.
// - NAV: release after 1~4s -> HOME (only if not in target-select sub-mode).
// - NAV target-select sub-mode (entered by a short tap on NAV when the
//   candidate list is non-empty): each subsequent short tap cycles to the
//   next candidate; hold 3s to confirm and start tracking; 8s of inactivity
//   exits without changes. While selecting, sleep (5s hold) and SOS (triple
//   tap) are gated off — release the button to exit select mode first.
// - Any non–Power-OFF screen (HOME / NAV / pairing / SOS): hold 5s -> Power OFF.
// - SOS: hold 3s -> back to previous screen (if released before 5s global sleep).
// - Power OFF UI: hold 3s wake -> HOME; hold 12s -> factory clear NVS, then HOME.
// - HOME / NAV: triple short press within 2s -> SOS (NAV gated off while selecting).
//
#define UI_HOLD_SLEEP_MS            5000UL
#define UI_HOLD_WAKE_FROM_SLEEP_MS  3000UL
#define UI_HOLD_NAV_FROM_HOME_MS    500UL
#define UI_HOLD_HOME_FROM_NAV_MS    1000UL
#define UI_HOLD_PAIR_FROM_HOME_MS   1000UL
#define UI_HOLD_EXIT_PAIR_MS        1000UL   // leave pairing screen -> HOME
#define UI_HOLD_HOME_PAIR_MAX_MS    4000UL   // home<->pairing/nav switch upper bound (exclusive)
#define UI_HOLD_EXIT_SOS_MS         3000UL
#define UI_HOLD_FACTORY_FROM_SLEEP_MS 12000UL

#define UI_TRIPLE_WINDOW_MS         2000UL  // all 3 short presses must complete within this window
#define UI_SHORT_PRESS_MAX_MS       650UL   // treat release as "short" if <= this

// Target-select sub-mode of NAV: short tap = cycle, hold 3s = confirm, idle 8s = cancel.
#define UI_SELECT_CONFIRM_MS        3000UL
#define UI_SELECT_IDLE_TIMEOUT_MS   8000UL
#define UI_SELECT_CONFIRM_FLASH_MS  600UL

// ---- ESP-NOW ----
uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---- BLE Configuration ----
#define BLE_SERVICE_UUID         "12345678-1234-5678-1234-56781234abcd"
#define BLE_MAC_CHAR_UUID        "12345678-1234-5678-1234-56781234abce"
#define BLE_OWNER_WRITE_CHAR_UUID "12345678-1234-5678-1234-56781234abcf"
#define BLE_OWNER_AUTH_CHAR_UUID  "12345678-1234-5678-1234-56781234abd0"
#define BLE_LOCATION_CHAR_UUID    "12345678-1234-5678-1234-56781234abd1"
#define BLE_TARGET_CHAR_UUID      "12345678-1234-5678-1234-56781234abd2"
#define BLE_PEER_LOCATION_CHAR_UUID "12345678-1234-5678-1234-56781234abd3"
#define BLE_CANDIDATES_CHAR_UUID  "12345678-1234-5678-1234-56781234abd4"
#define BLE_SELECTED_CHAR_UUID    "12345678-1234-5678-1234-56781234abd5"
#define TARGET_NAME_MAX_LEN       24
#define MAX_CANDIDATES            8
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

// SparkFun Example5 pattern: PBA + rates[] moving average (see `heartRate.h/.cpp`).
#define HR_SAMPLES_PER_LOOP  20
#define HR_FINGER_THRESHOLD 50000
#define HR_RATE_SIZE         4
#define HR_DELTA_MIN_MS      300UL
#define HR_DELTA_MAX_MS      1800UL
static uint8_t hrRates[HR_RATE_SIZE];
static uint8_t hrRateSpot = 0;
static unsigned long hrLastBeatMs = 0;
int currentBPM = 0;
bool fingerDetected = false;

static void hrOnBeatDetected() {
  unsigned long now = millis();
  if (hrLastBeatMs != 0) {
    unsigned long delta = now - hrLastBeatMs;
    if (delta >= HR_DELTA_MIN_MS && delta <= HR_DELTA_MAX_MS) {
      int bpm = (int)(60000.0f / (float)delta);
      if (bpm >= 35 && bpm <= 200) {
        hrRates[hrRateSpot++] = (uint8_t)constrain(bpm, 0, 255);
        hrRateSpot %= HR_RATE_SIZE;
        int sum = 0, n = 0;
        for (uint8_t i = 0; i < HR_RATE_SIZE; i++) {
          if (hrRates[i] > 0) {
            sum += hrRates[i];
            n++;
          }
        }
        currentBPM = (n > 0) ? (sum / n) : 0;
      }
    }
  }
  hrLastBeatMs = now;
}

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
NimBLECharacteristic* pCandidatesCharacteristic = nullptr;
NimBLECharacteristic* pSelectedCharacteristic = nullptr;
unsigned long lastLocationNotify = 0;
unsigned long lastPeerLocationNotify = 0;

// ---- Target selection state ----
bool     hasTarget         = false;
uint8_t  targetMac[6]      = {0};
char     targetName[TARGET_NAME_MAX_LEN + 1] = "";
char     targetBleName[10] = "";   // expected "SW-XXXX" for scan filtering

// ---- Candidate list (group members with linked devices) ----
struct Candidate {
  uint8_t mac[6];
  char    name[TARGET_NAME_MAX_LEN + 1];  // null-terminated, <=24 chars
};
static Candidate candidates[MAX_CANDIDATES];
static uint8_t   candidateCount = 0;

// "FF:FF:FF:FF:FF:FF" sentinel for "no target" on the …abd5 notify channel.
static const uint8_t kClearedMacBytes[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// ---- NAV target-select sub-mode ----
static bool          navSelectActive = false;
static int8_t        navSelectIndex = -1;
static unsigned long navSelectLastInteractionMs = 0;
static bool          navSelectConfirmLatched = false;     // 3s hold reached on this press
static unsigned long navSelectConfirmFlashUntil = 0;       // ms while we show "Tracking" flash
static bool          navSelectSuppressShortRelease = false; // ignore the release that crosses confirm

// ---- Persistent pairing state ----
Preferences prefs;
const char* PREFS_NAMESPACE = "sw-pair";
const char* PREFS_KEY_PAIRED = "paired";
const char* PREFS_KEY_OWNER  = "owner";
const char* PREFS_KEY_TMAC   = "tmac";
const char* PREFS_KEY_TNAME  = "tname";
const char* PREFS_KEY_CANDS  = "cands";  // packed candidate list (group members with linked devices)

bool     isPaired        = false;
String   ownerId         = "";          // Supabase user_id (UUID, 36 chars)
bool     pairingMode     = false;       // true only after "Hold 3s" from HOME (or legacy paths)
unsigned long pairingEnteredAt = 0;

// Per-connection auth: central must write owner-auth within AUTH_GRACE_MS.
bool     authedConn       = false;
unsigned long connectedAt = 0;
uint16_t currentConnHandle = 0xFFFF;
bool     hasActiveConn    = false;

// ---- UI overlay screens (LCD) ----
enum UiScreen : uint8_t {
  UI_NAV = 0,
  UI_SLEEP = 1,
  UI_SOS = 2,
  UI_HOME = 3,
};

static UiScreen uiScreen = UI_HOME;
static UiScreen uiBeforeSos = UI_HOME;     // restore target after SOS exit
static bool uiShowPairingAfterWake = false; // visual pairing screen after sleep wake (paired devices)
static bool uiSleepLatched = false;        // 5s -> sleep (once per hold)
static bool uiWakeLatched = false;          // 3s wake from Power OFF screen (once per hold)
static bool uiSosExitLatched = false;       // 3s exit SOS (once per hold)
static bool uiFactoryLatched = false;       // 12s factory reset from sleep (once per hold)
static bool uiPairFromHomeLatched = false;   // 3s -> BLE pairing from HOME (once per hold)
static bool uiNavBackHomeLatched = false;    // 3s -> HOME from NAV (once per hold)
static bool uiHomeToNavLatchedThisHold = false; // after HOME->NAV, suppress other hold gestures until release
static bool uiNavToHomeLatchedThisHold = false; // after NAV->HOME, suppress HOME hold gestures until release
static bool uiDeferPairingExitUntilBtnRelease = false;  // set when opening pairing from HOME; clear on release

// Triple-press detection (short presses)
static uint8_t uiShortPressCount = 0;
static unsigned long uiTripleWindowStartMs = 0;

// Button edge tracking
static int uiPrevBtnLevel = HIGH;
static unsigned long uiBtnDownSince = 0;

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

  // Pull fresh FIFO samples (SparkFun driver). Finger presence should be judged
  // from the same IR stream we feed into `checkForBeat()` (FIFO), not `getIR()`.
  particleSensor.safeCheck(2);

  long irWindow[HR_SAMPLES_PER_LOOP];
  int irCount = 0;
  long irMax = 0;

  for (int s = 0; s < HR_SAMPLES_PER_LOOP; s++) {
    if (particleSensor.available() == 0) {
      particleSensor.safeCheck(2);
      if (particleSensor.available() == 0) break;
    }

    long ir = (long)particleSensor.getFIFOIR();
    irWindow[irCount++] = ir;
    if (ir > irMax) irMax = ir;
    particleSensor.nextSample();
  }

  if (irCount == 0) {
    irMax = (long)particleSensor.getIR();
  }

  fingerDetected = (irMax > HR_FINGER_THRESHOLD);

  if (!fingerDetected) {
    currentBPM = 0;
    hrRateSpot = 0;
    hrLastBeatMs = 0;
    memset(hrRates, 0, sizeof(hrRates));
    return;
  }

  for (int i = 0; i < irCount; i++) {
    if (checkForBeat((int32_t)irWindow[i])) {
      hrOnBeatDetected();
    }
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
//  ESP-NOW CALLBACKS
// =====================================================

void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("[ESP-NOW] Send FAIL");
  }
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(PeerMessage)) return;
  // Filter: only accept packets from the selected target. Without a target,
  // ignore everything so the UI can show "Not tracking".
  if (!hasTarget) return;
  if (memcmp(info->src_addr, targetMac, 6) != 0) return;
  memcpy(&peerMsg, data, sizeof(PeerMessage));
  peerReceived = true;
  lastPeerTime = millis();
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

// Push the currently-selected target MAC to subscribed centrals via …abd5.
// All-0xFF means "no target". Source of truth for iOS Group screen badge.
static void notifySelected(const uint8_t mac[6]) {
  if (!pSelectedCharacteristic) return;
  pSelectedCharacteristic->setValue((uint8_t*)mac, 6);
  pSelectedCharacteristic->notify();
  Serial.printf("[BLE] Selected notify: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Pack the in-RAM candidate list into the wire format and persist it to NVS.
// Wire format: [u8 count] then for each: [6 MAC][u8 nameLen][nameLen UTF-8].
static void persistCandidates() {
  uint8_t buf[1 + MAX_CANDIDATES * (6 + 1 + TARGET_NAME_MAX_LEN)];
  size_t off = 0;
  buf[off++] = candidateCount;
  for (uint8_t i = 0; i < candidateCount; i++) {
    memcpy(buf + off, candidates[i].mac, 6); off += 6;
    uint8_t nl = (uint8_t)strnlen(candidates[i].name, TARGET_NAME_MAX_LEN);
    buf[off++] = nl;
    memcpy(buf + off, candidates[i].name, nl); off += nl;
  }
  prefs.begin(PREFS_NAMESPACE, false);
  if (candidateCount == 0) {
    prefs.remove(PREFS_KEY_CANDS);
  } else {
    prefs.putBytes(PREFS_KEY_CANDS, buf, off);
  }
  prefs.end();
}

// Load and parse the persisted candidate list. Silently zeroes the in-RAM
// list on malformed payloads so a corrupt write can't brick the watch.
static void loadCandidates() {
  candidateCount = 0;
  prefs.begin(PREFS_NAMESPACE, true);
  size_t got = prefs.getBytesLength(PREFS_KEY_CANDS);
  if (got == 0 || got > sizeof(uint8_t) + MAX_CANDIDATES * (6 + 1 + TARGET_NAME_MAX_LEN)) {
    prefs.end();
    return;
  }
  uint8_t buf[1 + MAX_CANDIDATES * (6 + 1 + TARGET_NAME_MAX_LEN)];
  prefs.getBytes(PREFS_KEY_CANDS, buf, got);
  prefs.end();

  size_t off = 0;
  if (off >= got) return;
  uint8_t count = buf[off++];
  if (count > MAX_CANDIDATES) return;

  for (uint8_t i = 0; i < count; i++) {
    if (off + 7 > got) return;
    Candidate c{};
    memcpy(c.mac, buf + off, 6); off += 6;
    uint8_t nl = buf[off++];
    if (nl > TARGET_NAME_MAX_LEN || off + nl > got) return;
    memcpy(c.name, buf + off, nl);
    c.name[nl] = '\0';
    off += nl;
    candidates[i] = c;
  }
  candidateCount = count;
  Serial.printf("[CANDS] Loaded %u candidates from NVS\n", candidateCount);
}

// Returns true iff the given MAC matches one of the current candidates.
static bool isMacInCandidates(const uint8_t mac[6]) {
  for (uint8_t i = 0; i < candidateCount; i++) {
    if (memcmp(candidates[i].mac, mac, 6) == 0) return true;
  }
  return false;
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
  loadCandidates();
  // If the persisted active target is no longer in our candidate list, drop it
  // on boot so the watch comes up showing "Not tracking" instead of locking
  // onto a peer that left the group.
  if (hasTarget && candidateCount > 0 && !isMacInCandidates(targetMac)) {
    Serial.println("[PAIR] Persisted target not in candidate list — clearing");
    hasTarget = false;
    memset(targetMac, 0, 6);
    targetName[0] = '\0';
    targetBleName[0] = '\0';
    prefs.begin(PREFS_NAMESPACE, false);
    prefs.remove(PREFS_KEY_TMAC);
    prefs.remove(PREFS_KEY_TNAME);
    prefs.end();
  }
  Serial.printf("[PAIR] Loaded: paired=%d owner=%s target=%s name=%s cands=%u\n",
                isPaired, ownerId.c_str(),
                hasTarget ? targetBleName : "(none)", targetName, candidateCount);
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
  notifySelected(targetMac);
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
  notifySelected(kClearedMacBytes);
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

// Factory / wipe: clears NVS owner + target. User opens real BLE pairing from HOME (3s hold).
void enterPairingMode(const char* reason) {
  Serial.printf("[PAIR] === ENTER PAIRING MODE (%s) ===\n", reason);
  clearPairing();
  uiShowPairingAfterWake = false;
  pairingMode = true;
  uiDeferPairingExitUntilBtnRelease = false;
  pairingEnteredAt = millis();
  authedConn = false;
  uiScreen = UI_NAV;  // shell for sleep/SOS gestures while pairing screen is shown
  forceRedraw = true;
  if (pBleServer && hasActiveConn) {
    pBleServer->disconnect(currentConnHandle);
  }
  if (bleActive) {
    NimBLEDevice::startAdvertising();
  }
}

void exitPairingMode(const String& uid) {
  savePairing(uid);
  uiShowPairingAfterWake = false;
  pairingMode = false;
  uiDeferPairingExitUntilBtnRelease = false;
  authedConn = true;                  // the connection that just wrote the owner is trusted
  uiScreen = UI_NAV;
  forceRedraw = true;
  Serial.println("[PAIR] === EXIT PAIRING MODE (linked) ===");
}

// BLE pairing UI + owner-write path, without wiping NVS (started from HOME, 3s hold).
void openBlePairingFromHome() {
  uiShowPairingAfterWake = false;
  pairingMode = true;
  pairingEnteredAt = millis();
  authedConn = false;
  uiScreen = UI_NAV;  // shell for sleep/SOS gestures while pairing screen is shown
  uiDeferPairingExitUntilBtnRelease = true;  // until release: ignore release-based cancel (same press as enter)
  if (pBleServer && hasActiveConn) {
    pBleServer->disconnect(currentConnHandle);
  }
  if (bleActive) {
    NimBLEDevice::startAdvertising();
  }
  forceRedraw = true;
  Serial.println("[PAIR] Opened BLE pairing from Home (owner-write enabled)");
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
      uiShowPairingAfterWake = false;
      if (uiScreen == UI_HOME) {
        uiScreen = UI_NAV;
      }
      forceRedraw = true;
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

// Replace the in-RAM candidate list, persist it, prune a stale active target
// (auto-clear + notify if the current target dropped out), and tickle the
// select-mode index if the wearer is mid-cycle. Returns true on success.
static bool applyCandidateUpdate(const uint8_t* data, size_t len) {
  if (len < 1) return false;
  uint8_t count = data[0];
  if (count > MAX_CANDIDATES) return false;

  Candidate next[MAX_CANDIDATES] = {};
  size_t off = 1;
  for (uint8_t i = 0; i < count; i++) {
    if (off + 7 > len) return false;
    memcpy(next[i].mac, data + off, 6); off += 6;
    uint8_t nl = data[off++];
    if (nl > TARGET_NAME_MAX_LEN || off + nl > len) return false;
    memcpy(next[i].name, data + off, nl);
    next[i].name[nl] = '\0';
    off += nl;
  }

  candidateCount = count;
  for (uint8_t i = 0; i < count; i++) candidates[i] = next[i];
  persistCandidates();

  // Prune the active target if it's no longer in the new list.
  if (hasTarget && !isMacInCandidates(targetMac)) {
    clearTarget();  // calls notifySelected(kClearedMacBytes)
  }

  // If the wearer is actively cycling, clamp the index to the new range.
  if (navSelectActive) {
    if (candidateCount == 0) {
      navSelectActive = false;
    } else if (navSelectIndex >= (int8_t)candidateCount) {
      navSelectIndex = candidateCount - 1;
    }
    forceRedraw = true;
  }

  Serial.printf("[CANDS] Applied %u candidates (auth=%d)\n", candidateCount, authedConn ? 1 : 0);
  return true;
}

class CandidatesWriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    if (!isPaired || !authedConn) {
      Serial.println("[BLE] candidates-write rejected: not authed");
      return;
    }
    std::string v = c->getValue();
    if (v.empty()) return;
    if (!applyCandidateUpdate((const uint8_t*)v.data(), v.size())) {
      Serial.printf("[BLE] candidates-write rejected: malformed (%u bytes)\n", (unsigned)v.size());
    }
  }
};

static SWServerCallbacks   serverCallbacks;
static OwnerWriteCallbacks ownerWriteCallbacks;
static OwnerAuthCallbacks  ownerAuthCallbacks;
static MacReadCallbacks    macReadCallbacks;
static TargetWriteCallbacks targetWriteCallbacks;
static CandidatesWriteCallbacks candidatesWriteCallbacks;

// =====================================================
//  BUTTON — sleep / SOS / wake (D9 / GPIO8 tactile -> GND)
// =====================================================

// Apply the candidate at navSelectIndex as the active target. Called when
// the wearer holds the button for UI_SELECT_CONFIRM_MS while in select mode.
static void applySelectedAsTarget() {
  if (navSelectIndex < 0 || navSelectIndex >= (int8_t)candidateCount) return;
  const Candidate& c = candidates[navSelectIndex];
  saveTarget(c.mac, c.name);  // persists, recomputes BLE name, fires notifySelected
  navSelectConfirmFlashUntil = millis() + UI_SELECT_CONFIRM_FLASH_MS;
  navSelectConfirmLatched = true;
  navSelectSuppressShortRelease = true;  // the release that ends the confirm hold should NOT cycle
  forceRedraw = true;
  Serial.printf("[SELECT] Confirmed -> %s\n", c.name);
}

static void exitNavSelectMode(const char* reason) {
  if (!navSelectActive && navSelectConfirmFlashUntil == 0) return;
  navSelectActive = false;
  navSelectIndex = -1;
  navSelectConfirmLatched = false;
  navSelectConfirmFlashUntil = 0;
  forceRedraw = true;
  Serial.printf("[SELECT] Exit (%s)\n", reason);
}

static void enterSleepUiOverlay() {
  uiSleepLatched = true;
  pairingMode = false;
  uiShowPairingAfterWake = false;
  uiScreen = UI_SLEEP;
  uiShortPressCount = 0;
  uiWakeLatched = false;
  uiSosExitLatched = false;
  uiFactoryLatched = false;
  uiPairFromHomeLatched = false;
  uiNavBackHomeLatched = false;
  uiDeferPairingExitUntilBtnRelease = false;
  navSelectActive = false;
  navSelectIndex = -1;
  navSelectConfirmLatched = false;
  navSelectConfirmFlashUntil = 0;
  forceRedraw = true;
  uiBtnDownSince = 0;  // same physical hold must not instantly satisfy wake threshold
  Serial.println("[UI] Enter SLEEP (hold 5s)");
}

static void armSosFromTripleTap() {
  if (uiScreen == UI_SOS) return;
  uiShowPairingAfterWake = false;
  uiBeforeSos = uiScreen;
  uiScreen = UI_SOS;
  uiShortPressCount = 0;
  uiTripleWindowStartMs = 0;
  uiSleepLatched = false;
  uiWakeLatched = false;
  uiSosExitLatched = false;
  uiFactoryLatched = false;
  uiPairFromHomeLatched = false;
  uiNavBackHomeLatched = false;
  uiDeferPairingExitUntilBtnRelease = false;
  forceRedraw = true;
  Serial.println("[UI] Enter SOS (triple tap)");
}

void updateUiButton() {
  const int level = digitalRead(PAIR_BUTTON_PIN);  // active-LOW
  const unsigned long now = millis();

  // Held: Power OFF wake/factory, global 5s Power OFF, SOS 3s exit, HOME/NAV long-holds
  if (level == LOW) {
    if (uiBtnDownSince == 0) uiBtnDownSince = now;
    const unsigned long held = now - uiBtnDownSince;

    if (uiScreen == UI_SLEEP) {
      if (!uiFactoryLatched && held >= UI_HOLD_FACTORY_FROM_SLEEP_MS) {
        uiFactoryLatched = true;
        enterPairingMode("sleep-hold-factory");
        uiWakeLatched = false;
        uiBtnDownSince = 0;
        forceRedraw = true;
        Serial.println("[UI] FACTORY reset from sleep (hold 12s)");
      } else if (!uiWakeLatched && held >= UI_HOLD_WAKE_FROM_SLEEP_MS) {
        uiWakeLatched = true;
        uiScreen = UI_HOME;
        uiSleepLatched = false;
        uiFactoryLatched = false;
        pairingMode = false;
        uiShowPairingAfterWake = false;
        uiDeferPairingExitUntilBtnRelease = false;
        forceRedraw = true;
        Serial.println("[UI] Wake from Power OFF (hold 3s) -> HOME");
        uiBtnDownSince = 0;
      }
    } else if (!uiSleepLatched && held >= UI_HOLD_SLEEP_MS && !navSelectActive) {
      // Power OFF from any other screen (HOME / NAV / pairing / SOS).
      // While in NAV target-select sub-mode, sleep is gated — release first
      // to exit select mode, then re-hold 5s.
      enterSleepUiOverlay();
    } else if (uiScreen == UI_SOS) {
      if (!uiSosExitLatched && held >= UI_HOLD_EXIT_SOS_MS) {
        uiSosExitLatched = true;
        uiScreen = uiBeforeSos;
        uiShortPressCount = 0;
        uiSleepLatched = false;
        uiWakeLatched = false;
        uiFactoryLatched = false;
        uiPairFromHomeLatched = false;
        uiDeferPairingExitUntilBtnRelease = false;
        forceRedraw = true;
        Serial.println("[UI] Exit SOS (hold 3s)");
        uiBtnDownSince = 0;
      }
    } else if (uiScreen == UI_HOME) {
      // HOME->NAV and HOME->pairing are decided on release windows.
    } else if (uiScreen == UI_NAV) {
      // NAV->HOME is decided on release window [1s,3s).
      // While in target-select sub-mode, hold UI_SELECT_CONFIRM_MS = confirm.
      if (navSelectActive && !navSelectConfirmLatched && held >= UI_SELECT_CONFIRM_MS) {
        applySelectedAsTarget();
      }
    }
  } else {
    // Released: edge detect + triple-tap windowing
    if (uiPrevBtnLevel == LOW) {
      const unsigned long downMs = (uiBtnDownSince == 0) ? 0 : (now - uiBtnDownSince);
      const bool shortPress = (downMs > 0) && (downMs <= UI_SHORT_PRESS_MAX_MS);

      // Leave pairing without Power OFF: release in [1s, 4s) (not while defer-from-home is active).
      if (pairingMode && !uiDeferPairingExitUntilBtnRelease &&
          downMs >= UI_HOLD_EXIT_PAIR_MS && downMs < UI_HOLD_HOME_PAIR_MAX_MS) {
        pairingMode = false;
        uiShowPairingAfterWake = false;
        uiScreen = UI_HOME;
        if (pBleServer && hasActiveConn) {
          pBleServer->disconnect(currentConnHandle);
        }
        forceRedraw = true;
        Serial.println("[UI] Exit pairing -> HOME (release after 1s, before 4s)");
      } else if (uiScreen == UI_HOME && !uiNavToHomeLatchedThisHold &&
                 downMs >= UI_HOLD_NAV_FROM_HOME_MS &&
                 downMs < UI_HOLD_PAIR_FROM_HOME_MS) {
        uiScreen = UI_NAV;
        uiShortPressCount = 0;
        uiTripleWindowStartMs = 0;
        forceRedraw = true;
        Serial.println("[UI] HOME -> NAV (release after 0.5s, before 1s)");
      } else if (uiScreen == UI_HOME && !uiNavToHomeLatchedThisHold &&
                 downMs >= UI_HOLD_PAIR_FROM_HOME_MS &&
                 downMs < UI_HOLD_HOME_PAIR_MAX_MS) {
        openBlePairingFromHome();
        Serial.println("[UI] HOME -> pairing (release after 1s, before 4s)");
      } else if (uiScreen == UI_NAV && !navSelectActive &&
                 downMs >= UI_HOLD_HOME_FROM_NAV_MS &&
                 downMs < UI_HOLD_HOME_PAIR_MAX_MS) {
        uiNavBackHomeLatched = true;
        uiScreen = UI_HOME;
        uiShortPressCount = 0;
        uiTripleWindowStartMs = 0;
        uiNavToHomeLatchedThisHold = true;
        exitNavSelectMode("nav-back-home");
        forceRedraw = true;
        Serial.println("[UI] NAV -> HOME (release after 1s, before 4s)");
      } else if (navSelectConfirmLatched || navSelectSuppressShortRelease) {
        // Release that ended a 3s confirm hold — already applied; do not cycle.
        // Exit select mode but keep the flash timer running so the watch shows
        // a brief "Tracking <name>" confirmation before returning to NAV.
        navSelectConfirmLatched = false;
        navSelectSuppressShortRelease = false;
        navSelectActive = false;
        navSelectIndex = -1;
        navSelectLastInteractionMs = now;
        uiShortPressCount = 0;
        uiTripleWindowStartMs = 0;
        forceRedraw = true;
      } else if (shortPress && uiScreen == UI_NAV && navSelectActive) {
        // Cycle to next candidate. Triple-tap SOS is gated off while selecting.
        if (candidateCount > 0) {
          navSelectIndex = (navSelectIndex + 1) % (int8_t)candidateCount;
        }
        navSelectLastInteractionMs = now;
        uiShortPressCount = 0;
        uiTripleWindowStartMs = 0;
        forceRedraw = true;
        Serial.printf("[SELECT] Cycle -> %d/%u\n", (int)navSelectIndex + 1, candidateCount);
      } else if (shortPress && uiScreen == UI_NAV && !navSelectActive && candidateCount > 0) {
        // Enter select mode on a quick tap when there are candidates. The first
        // tap acts as "show first candidate" so cycling N taps lands on N-th.
        navSelectActive = true;
        navSelectIndex = 0;
        // Pre-seed the index so currently-tracked target appears first if present.
        if (hasTarget) {
          for (uint8_t i = 0; i < candidateCount; i++) {
            if (memcmp(candidates[i].mac, targetMac, 6) == 0) {
              navSelectIndex = (int8_t)i;
              break;
            }
          }
        }
        navSelectLastInteractionMs = now;
        uiShortPressCount = 0;
        uiTripleWindowStartMs = 0;
        forceRedraw = true;
        Serial.printf("[SELECT] Enter -> %d/%u\n", (int)navSelectIndex + 1, candidateCount);
      } else if (shortPress && (uiScreen == UI_NAV || uiScreen == UI_HOME)) {
        if (uiShortPressCount == 0) {
          uiShortPressCount = 1;
          uiTripleWindowStartMs = now;
        } else {
          if (now - uiTripleWindowStartMs > UI_TRIPLE_WINDOW_MS) {
            // Window expired; restart counting from this press.
            uiShortPressCount = 1;
            uiTripleWindowStartMs = now;
          } else {
            uiShortPressCount++;
          }
        }

        if (uiShortPressCount >= 3 && (now - uiTripleWindowStartMs) <= UI_TRIPLE_WINDOW_MS) {
          armSosFromTripleTap();
        }
      } else if (shortPress && (uiScreen == UI_SLEEP || uiScreen == UI_SOS)) {
        // Don't interpret short taps as triple-press while overlays are active.
        uiShortPressCount = 0;
        uiTripleWindowStartMs = 0;
      } else {
        // Long / ambiguous release: reset triple counter
        uiShortPressCount = 0;
        uiTripleWindowStartMs = 0;
      }
    }

    uiBtnDownSince = 0;
    uiPrevBtnLevel = HIGH;

    // Allow repeating holds after release
    uiSleepLatched = false;
    uiWakeLatched = false;
    uiSosExitLatched = false;
    uiFactoryLatched = false;
    uiPairFromHomeLatched = false;
    uiNavBackHomeLatched = false;
    uiHomeToNavLatchedThisHold = false;
    uiNavToHomeLatchedThisHold = false;
    uiDeferPairingExitUntilBtnRelease = false;
    return;
  }

  uiPrevBtnLevel = level;
}

// Drive the NAV target-select sub-mode timeouts: 8s idle exit, ~600ms
// confirmation flash before returning to normal NAV.
void updateNavSelect() {
  unsigned long now = millis();
  if (navSelectConfirmFlashUntil != 0 && now >= navSelectConfirmFlashUntil) {
    navSelectConfirmFlashUntil = 0;
    forceRedraw = true;
  }
  if (navSelectActive && uiBtnDownSince == 0 &&
      navSelectLastInteractionMs > 0 &&
      now - navSelectLastInteractionMs > UI_SELECT_IDLE_TIMEOUT_MS) {
    exitNavSelectMode("idle-timeout");
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

  // Candidate list: iOS pushes up to 8 group members the wearer can cycle
  // through and confirm on the watch. Owner-auth gated.
  pCandidatesCharacteristic = pBleService->createCharacteristic(
    BLE_CANDIDATES_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE
  );
  pCandidatesCharacteristic->setCallbacks(&candidatesWriteCallbacks);

  // Selected target (read + notify). Source of truth for the iOS Group screen
  // badge. Fires on watch long-press confirm, on phone abd2 writes (mirrored),
  // and on stale-prune (FF×6 = cleared).
  pSelectedCharacteristic = pBleService->createCharacteristic(
    BLE_SELECTED_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  if (hasTarget) {
    pSelectedCharacteristic->setValue((uint8_t*)targetMac, 6);
  } else {
    pSelectedCharacteristic->setValue((uint8_t*)kClearedMacBytes, 6);
  }

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
  pCandidatesCharacteristic = nullptr;
  pSelectedCharacteristic = nullptr;
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

// Home watch face: blue ring, four dots at 11 / 2 / 4 / 6 o'clock, centered mono text over ring (UI mock).
static void drawHomeRingDot(int cx, int cy, int rOrbit, double deg, int dotRadius, uint16_t col) {
  const double rad = deg * (M_PI / 180.0);
  int px = (int)(cx + rOrbit * cos(rad) + 0.5);
  int py = (int)(cy - rOrbit * sin(rad) + 0.5);
  tft.fillCircle(px, py, dotRadius, col);
}

static void drawHomeScreen() {
  const int cx = 120;
  const int cy = 120;
  const int ringR = 96;
  const int orbitR = 94;
  const uint16_t ringBlue = 0x19DF;  // saturated blue (RGB565)
  const uint16_t dotPink = 0xFD55;    // salmon / light pink
  const uint16_t dotLav = 0xD59E;     // soft purple
  const uint16_t dotMint = 0x67F5;    // mint green
  const uint16_t dotCyan = COLOR_CYAN;

  tft.fillScreen(COLOR_BG);
  tft.drawCircle(cx, cy, ringR, ringBlue);

  // Dots at 11, 2, 4, 6 o'clock (angles CCW from +x; y grows downward).
  const int dr = 5;
  drawHomeRingDot(cx, cy, orbitR, 120.0, dr, dotPink);   // ~11 o'clock
  drawHomeRingDot(cx, cy, orbitR, 30.0, dr, dotLav);      // ~2 o'clock
  drawHomeRingDot(cx, cy, orbitR, -30.0, dr, dotMint);   // ~4 o'clock
  drawHomeRingDot(cx, cy, orbitR, 270.0, dr, dotCyan);   // 6 o'clock

  // FreeMono: white text centered over ring (drawn after ring so it sits on top).
  const char* lines[] = {"Hello!", "1s-Connecting", "3s-Pairing"};
  const int nHomeLines = (int)(sizeof(lines) / sizeof(lines[0]));
  tft.setFont(&FreeMono9pt7b);
  tft.setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;
  int totalH = 0;
  uint16_t lineH[4];
  int16_t firstAscentY1 = 0;
  for (int i = 0; i < nHomeLines; i++) {
    tft.getTextBounds(lines[i], 0, 0, &x1, &y1, &w, &lineH[i]);
    if (i == 0) firstAscentY1 = y1;
    totalH += (int)lineH[i] + (i < nHomeLines - 1 ? 3 : 0);
  }
  int baselineY = cy - totalH / 2 - (int)firstAscentY1;
  for (int i = 0; i < nHomeLines; i++) {
    tft.getTextBounds(lines[i], 0, 0, &x1, &y1, &w, &lineH[i]);
    tft.setTextColor(COLOR_WHITE, COLOR_BG);
    tft.setCursor((240 - (int)w) / 2, baselineY);
    tft.print(lines[i]);
    baselineY += (int)lineH[i] + 3;
  }
  tft.setFont(NULL);
  tft.setTextSize(1);
}

static void drawSleepScreen() {
  tft.fillScreen(COLOR_BG);
  drawCenteredText("Power", 100, COLOR_WHITE, 2);
  drawCenteredText("OFF", 128, COLOR_WHITE, 2);
}

static void drawSosScreen() {
  tft.fillScreen(COLOR_BG);
  drawCenteredText("SOS", 105, COLOR_RED, 3);
  drawCenteredText("Hold 3s to exit", 150, COLOR_GRAY, 1);
}

// Render the candidate name + "(X of N)" + hints. While the wearer is holding
// the button (uiBtnDownSince != 0), draw a cyan progress arc around the
// perimeter that fills from 0 -> 100% over UI_SELECT_CONFIRM_MS.
//
// To avoid flicker we only fillScreen on a forceRedraw (entry / cycle).
// Subsequent ticks during a hold draw new arc dots incrementally; on release
// without confirm, the next tick clears the arc by forcing a redraw.
static int s_prevArcSteps = 0;
static int8_t s_prevSelectIndexDrawn = -1;

static void drawTargetSelectScreen() {
  bool fullRedraw = forceRedraw || s_prevSelectIndexDrawn != navSelectIndex;

  if (fullRedraw) {
    tft.fillScreen(COLOR_BG);

    if (candidateCount == 0 || navSelectIndex < 0 || navSelectIndex >= (int8_t)candidateCount) {
      drawCenteredText("No candidates", 110, COLOR_GRAY, 2);
      drawPhoneIcon(hasActiveConn && authedConn);
      s_prevArcSteps = 0;
      s_prevSelectIndexDrawn = navSelectIndex;
      return;
    }

    const Candidate& c = candidates[navSelectIndex];
    char nameBuf[TARGET_NAME_MAX_LEN + 1];
    strncpy(nameBuf, c.name, sizeof(nameBuf) - 1);
    nameBuf[sizeof(nameBuf) - 1] = '\0';
    uint8_t titleSize = strlen(nameBuf) > 10 ? 1 : 2;
    drawCenteredText(nameBuf[0] ? nameBuf : "Friend", 75, COLOR_CYAN, titleSize);

    char ofBuf[16];
    snprintf(ofBuf, sizeof(ofBuf), "(%d of %u)", (int)navSelectIndex + 1, candidateCount);
    drawCenteredText(ofBuf, 110, COLOR_WHITE, 1);

    drawCenteredText("Tap to cycle", 145, COLOR_GRAY, 1);
    drawCenteredText("Hold 3s to confirm", 162, COLOR_GRAY, 1);

    drawPhoneIcon(hasActiveConn && authedConn);
    s_prevArcSteps = 0;
    s_prevSelectIndexDrawn = navSelectIndex;
  }

  // Progress arc — incremental.
  int steps = 0;
  if (uiBtnDownSince != 0) {
    unsigned long held = millis() - uiBtnDownSince;
    if (held > UI_SELECT_CONFIRM_MS) held = UI_SELECT_CONFIRM_MS;
    steps = (int)((held * 96) / UI_SELECT_CONFIRM_MS);
  }
  if (steps > s_prevArcSteps) {
    const int cx = 120, cy = 120, r = 110;
    for (int i = s_prevArcSteps; i < steps; i++) {
      double deg = -90.0 + ((double)i / 96.0) * 360.0;
      double rad = deg * (M_PI / 180.0);
      int px = (int)(cx + r * cos(rad) + 0.5);
      int py = (int)(cy + r * sin(rad) + 0.5);
      tft.fillCircle(px, py, 2, COLOR_CYAN);
    }
    s_prevArcSteps = steps;
  } else if (steps < s_prevArcSteps && !fullRedraw) {
    // Button released without confirm — clear arc by forcing redraw next tick.
    s_prevArcSteps = 0;
    forceRedraw = true;
  }
}

// Brief "Tracking <name>" overlay shown for ~600ms after long-press confirm.
static void drawConfirmFlashScreen() {
  tft.fillScreen(COLOR_BG);
  drawCenteredText("Tracking", 90, COLOR_GREEN, 2);
  if (targetName[0]) {
    char buf[TARGET_NAME_MAX_LEN + 1];
    strncpy(buf, targetName, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    drawCenteredText(buf, 130, COLOR_WHITE, 2);
  }
  // Full cyan ring at perimeter for visual punch.
  tft.drawCircle(120, 120, 110, COLOR_CYAN);
  tft.drawCircle(120, 120, 109, COLOR_CYAN);
}

void updateDisplay() {
  // Global overlays take precedence over pairing/nav UIs.
  if (uiScreen == UI_SLEEP) {
    if (forceRedraw) {
      drawSleepScreen();
      forceRedraw = false;
      prevDisplayDist = -999;
      prevDisplayMode = 255;
      prevPhoneLinked = false;
    }
    return;
  }

  if (uiScreen == UI_SOS) {
    if (forceRedraw) {
      drawSosScreen();
      forceRedraw = false;
      prevDisplayDist = -999;
      prevDisplayMode = 255;
      prevPhoneLinked = false;
    }
    return;
  }

  if (uiScreen == UI_HOME) {
    if (forceRedraw) {
      drawHomeScreen();
      forceRedraw = false;
      prevDisplayDist = -999;
      prevDisplayMode = 255;
      prevPhoneLinked = false;
    }
    return;
  }

  // Confirm-flash overlay takes precedence over normal NAV but is shown only
  // briefly after a long-press confirm. After it expires, normal NAV resumes.
  if (uiScreen == UI_NAV && navSelectConfirmFlashUntil != 0 && millis() < navSelectConfirmFlashUntil) {
    if (forceRedraw) {
      drawConfirmFlashScreen();
      forceRedraw = false;
      prevDisplayDist = -999;
      prevDisplayMode = 255;
      prevPhoneLinked = false;
    }
    return;
  }

  // Target-select sub-mode of NAV.
  if (uiScreen == UI_NAV && navSelectActive) {
    // Redraw frequently so the progress arc animates while held.
    drawTargetSelectScreen();
    forceRedraw = false;
    prevDisplayDist = -999;
    prevDisplayMode = 255;
    prevPhoneLinked = (hasActiveConn && authedConn);
    return;
  }

  if (pairingMode || uiShowPairingAfterWake) {
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
  Serial.println("  Spatial Wearable — Group Targets (16)");
  Serial.println("==========================================");

  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  loadPairing();
  pairingMode = false;
  uiScreen = UI_HOME;
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

  pinMode(PAIR_BUTTON_PIN, INPUT_PULLUP);
  Serial.printf("[UI] Button on GPIO%d (board D9), active-LOW to GND — not D10 (MOSI)\n", PAIR_BUTTON_PIN);

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

    PeerMessage msg;
    msg.gpsValid = gps.location.isValid();
    msg.lat = msg.gpsValid ? gps.location.lat() : 0;
    msg.lng = msg.gpsValid ? gps.location.lng() : 0;
    msg.satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
    msg.mode = currentMode;
    msg.txPower = 9;

    esp_now_send(broadcastAddr, (uint8_t *)&msg, sizeof(msg));

    bool gpsFix = gps.location.isValid();
    const int btnPin = digitalRead(PAIR_BUTTON_PIN);  // INPUT_PULLUP: LOW = pressed
    Serial.printf("[STATUS] Mode:%s | Dist:%.1fm | BLE:%.1fm | GPS:%.1fm | BPM:%d | Finger:%s | Btn:%s\n",
      currentMode == MODE_BLE ? "BLE" : "GPS",
      getActiveDistance(),
      bleDistance,
      gpsDistance,
      currentBPM,
      fingerDetected ? "Y" : "N",
      btnPin == LOW ? "PRS" : "REL");
    Serial.printf("[GPS] Fix:%s | Lat:%.6f | Lng:%.6f | Sats:%d\n",
      gpsFix ? "YES" : "NO",
      gpsFix ? gps.location.lat() : 0.0,
      gpsFix ? gps.location.lng() : 0.0,
      gps.satellites.isValid() ? gps.satellites.value() : 0);
  }

  updateUiButton();
  updateNavSelect();
  updateConnAuthTimeout();

  // Auto-exit pairing mode after a long idle to avoid leaving the device
  // advertising as un-owned forever.
  if (pairingMode && !isPaired && millis() - pairingEnteredAt > PAIR_MODE_TIMEOUT) {
    pairingMode = false;
    uiDeferPairingExitUntilBtnRelease = false;
    uiScreen = UI_HOME;
    forceRedraw = true;
    Serial.println("[PAIR] Pairing idle timeout -> HOME");
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
