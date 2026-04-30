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
//  BEHAVIOR (firmware 19 — tracking-screen UI polish):
//    - All firmware-18 features intact (GPS-sync over abd6, group SOS,
//      candidate cycling, target select, peer location forwarding, persistent
//      pairing, …).
//    - NEW: when actively tracking a peer and the live distance becomes
//      unavailable (BLE timed out + GPS not yet/no-longer fresh), the
//      tracking screen no longer collapses to "NO PEER" + "---". Instead it
//      persists the last valid distance and replaces the status line with a
//      ticking "Updated Xs ago" / "Updated Xm ago" counter so the wearer
//      sees how stale the reading is. As soon as either radio (BLE or GPS)
//      produces a fresh distance again, the counter, status, and number
//      snap back to live. The fully-fresh "no peer ever" state still shows
//      "NO PEER" / "SEARCHING…" + "---" since there is nothing to persist.
//    - The persisted last-distance is reset whenever the tracked target
//      changes (saveTarget / clearTarget), so switching peers never shows
//      a stale number for the new selection.
//
//  BEHAVIOR (firmware 18 — GPS sync, retained):
//    - GPS-sync characteristic (…abd6) lets iOS request a focused fix.
//      The 2.4 GHz radio (BLE + ESP-NOW + WiFi) desenses the GNSS chipset's
//      L1 front end enough that satellites are visible but a fix never
//      converges. Reboot-based flow: iOS writes 0x01, firmware persists
//      `syncpend=true` and ESP.restart()s; setup() detects it pre-radio-init
//      and routes into runFocusedFixOnBoot() (display + GPS UART only) for
//      a 30 s countdown poll; result is persisted and the device reboots a
//      second time, then the latched-result path pushes it over abd6 once
//      iOS re-auths. Total blackout ~40 s; iOS's 60 s outer timeout and
//      DashboardView.reconnectLoop handle the BLE round-trip transparently.
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
//   exits without changes. Sleep (5s hold) is gated off while selecting —
//   release the button to exit select mode first. SOS is reachable from
//   select mode via a fast triple tap (see below); cycling at normal reading
//   pace will not trigger it.
// - Any non–Power-OFF screen (HOME / NAV / pairing / SOS): hold 5s -> Power OFF.
// - SOS: hold 3s -> back to previous screen (if released before 5s global sleep).
// - Power OFF UI: hold 3s wake -> HOME; hold 12s -> factory clear NVS, then HOME.
// - HOME / NAV (incl. select sub-mode): three short presses within 600ms ->
//   SOS. Tight window so deliberate panic taps escalate even mid-cycle, while
//   normal cycling cadence (>~250ms between taps) cannot.
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

#define UI_TRIPLE_WINDOW_MS          600UL  // all 3 short presses must complete within this window (tight so cycling at reading pace can't trip SOS)
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
#define BLE_GPS_SYNC_CHAR_UUID    "12345678-1234-5678-1234-56781234abd6"
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

// ---- ESP-NOW Messages ----
// Declared here (before the first function definition) so that Arduino's
// auto-prototype generator — which injects forward declarations just before
// the first user function — can see these types when synthesizing prototypes
// for handlers like handleSosMessage(... const SosMessage&).
typedef struct __attribute__((packed)) {
  float lat;
  float lng;
  bool gpsValid;
  uint8_t satellites;
  uint8_t mode;
  int8_t txPower;
} PeerMessage;

// 16 bytes — distinct length from the 12-byte PeerMessage so onDataRecv can
// dispatch by length. Magic "SS" + version are still validated as a sanity
// check. seq is monotonic per sender so a late-arriving cancel for an older
// SOS event can't extinguish a freshly-armed one.
typedef struct __attribute__((packed)) {
  uint8_t  magic[2];      // 'S','S'
  uint8_t  version;       // 0x01
  uint8_t  flags;         // bit0: 1=active, 0=cancel; bits 1-7 reserved
  uint8_t  senderMac[6];  // sender's WiFi STA MAC
  uint32_t seq;           // monotonic per sender
  uint8_t  reserved[2];   // reserved for future fields (severity, battery, etc.)
} SosMessage;
static_assert(sizeof(SosMessage) == 16, "SosMessage must be 16 bytes");
static_assert(sizeof(PeerMessage) != sizeof(SosMessage),
              "SosMessage and PeerMessage must have distinct sizes for length-dispatch");

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

// (PeerMessage and SosMessage are declared above the heart-rate function so
// Arduino's auto-prototype generator can see them when emitting prototypes.)

// ---- Objects ----
Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);
TinyGPSPlus gps;

// Per-constellation "satellites in view" parsers. GSV sentence field 3
// (0-indexed) = total sats in view for that talker. The ATGM336H emits all
// four families when its multi-GNSS mode is enabled (GP=GPS, BD=BeiDou,
// GL=GLONASS, GA=Galileo). Used during the focused-fix sync to log how many
// satellites are *visible* even before any of them get used in a fix —
// matches the diagnostic format in arduino/02_gps_test and
// arduino/gps_fix_tests/17_wearable_radios_off.
TinyGPSCustom gpsInView(gps, "GPGSV", 3);
TinyGPSCustom bdInView(gps, "BDGSV", 3);
TinyGPSCustom glInView(gps, "GLGSV", 3);
TinyGPSCustom gaInView(gps, "GAGSV", 3);

static int parseGsvCount(const char* s) {
  if (!s || !*s) return 0;
  return atoi(s);
}

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

// Last-valid-distance cache for the "Updated Xs ago" tracking-screen overlay
// (firmware 19). Populated whenever getActiveDistance() returns >= 0 while a
// target is selected; reset on saveTarget/clearTarget so a new peer never
// inherits the previous peer's stale number. While the live distance is
// unavailable but lastValidDistance >= 0, the tracking screen shows the
// last value plus a ticking "ago" counter instead of the old "NO PEER"/---
// collapse. prevAgoBucket lets us redraw the counter once per second
// without thrashing the rest of the screen.
double         lastValidDistance       = -1;
unsigned long  lastValidDistanceTime   = 0;
unsigned long  prevAgoBucket           = 0;
bool           prevStale               = false;

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
NimBLECharacteristic* pGpsSyncCharacteristic = nullptr;
unsigned long lastLocationNotify = 0;
unsigned long lastPeerLocationNotify = 0;

// ---- GPS sync state (firmware 18) ----
// Status byte exposed via the abd6 characteristic. Mirrors the iOS-side enum.
#define GPS_SYNC_IDLE     0x00
#define GPS_SYNC_RUNNING  0x01
#define GPS_SYNC_SUCCESS  0x02
#define GPS_SYNC_FAILED   0x03

#define GPS_SYNC_DURATION_MS  30000UL
#define GPS_SYNC_FRESH_AGE_MS 3000UL    // a "fresh" fix must be <3s old
#define GPS_SYNC_RESULT_TTL_MS 60000UL  // latched result expires after 60s
// Pause after queuing the "running" notify so the GATT response and notify
// PDU clear the air before NimBLE deinits. Empirically <100ms; 250 is safe.
#define GPS_SYNC_NOTIFY_FLUSH_MS 250UL

bool          gpsSyncTriggerPending = false;  // set in BLE callback, drained in loop()
bool          gpsSyncActive         = false;  // guards re-entry while running
uint8_t       gpsSyncResult         = GPS_SYNC_IDLE;
unsigned long gpsSyncResultAt       = 0;
bool          gpsSyncResultPushed   = true;   // true iff iOS has been notified of the latched result

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

// ---- SOS state (firmware 17) ----
// `sosActive` is set when this wearable is itself in SOS mode. While true, the
// 1 Hz ESP-NOW tick also sends an SosMessage with flag=active. `mySosSeq` is
// bumped on every entry so receivers can distinguish a fresh SOS from a stale
// cancel for a previous event.
bool          sosActive          = false;
uint32_t      mySosSeq           = 0;
unsigned long lastSosBroadcast   = 0;

// Tracks SOS alerts received from peers. Sized to MAX_CANDIDATES (8); the
// strict candidate-only RX filter means we never need more entries than the
// group size. lastHeardMs drives the 10 s liveness timeout, dismissedLocally
// suppresses the overlay & haptic for an entry without removing it (so a new
// flag=1 with a higher seq from the same peer can re-alert the wearer).
struct IncomingSos {
  bool          inUse;
  uint8_t       mac[6];
  uint32_t      lastSeq;
  unsigned long lastHeardMs;
  bool          dismissedLocally;
};
static IncomingSos incomingSos[MAX_CANDIDATES];

#define SOS_BROADCAST_INTERVAL_MS  1000UL
#define SOS_LIVENESS_TIMEOUT_MS   10000UL
#define SOS_CANCEL_BURST_COUNT     5
#define SOS_CANCEL_BURST_GAP_MS    80

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
// GPS sync (firmware 18). syncpend=true on boot means we should skip all
// radio init and go straight into the focused-fix loop. syncres holds the
// terminal status (GPS_SYNC_SUCCESS / GPS_SYNC_FAILED) the focused-fix
// loop wrote on its way out; the next clean boot picks it up and the
// existing pushLatchedGpsSyncResult() flow notifies iOS.
const char* PREFS_KEY_SYNC_PENDING = "syncpend";
const char* PREFS_KEY_SYNC_RESULT  = "syncres";

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
  // SOS owns the motor pin while either own SOS or any non-dismissed incoming
  // SOS is active. Drop our latched state so the next non-SOS tick starts
  // clean; updateSosHaptic() drives the pin in the meantime.
  if (sosActive || incomingSosActive()) {
    hapticOn = false;
    return;
  }

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
//  SOS BROADCAST (firmware 17)
// =====================================================

// Forward declaration — `isMacInCandidates` is static and defined later in
// the file (in the persistent-pairing section). Arduino's auto-prototype
// generator skips static functions, so we declare it explicitly here.
static bool isMacInCandidates(const uint8_t mac[6]);

// True iff at least one entry in incomingSos[] is in use and not yet locally
// dismissed by the wearer. Drives the haptic-priority guard and the overlay.
bool incomingSosActive() {
  for (uint8_t i = 0; i < MAX_CANDIDATES; i++) {
    if (incomingSos[i].inUse && !incomingSos[i].dismissedLocally) return true;
  }
  return false;
}

// Index of the newest non-dismissed entry, or -1 if none. "Newest" wins so a
// fresh peer SOS pre-empts the overlay for an older one; dismissing the
// newest reveals the next-newest in lastHeardMs order.
int latestIncomingSosIndex() {
  int best = -1;
  unsigned long bestMs = 0;
  for (uint8_t i = 0; i < MAX_CANDIDATES; i++) {
    if (!incomingSos[i].inUse || incomingSos[i].dismissedLocally) continue;
    if (best < 0 || incomingSos[i].lastHeardMs > bestMs) {
      best = i;
      bestMs = incomingSos[i].lastHeardMs;
    }
  }
  return best;
}

// Drop entries whose sender hasn't refreshed in SOS_LIVENESS_TIMEOUT_MS. This
// is the safety net for a sender that powered off mid-SOS without sending a
// cancel burst.
void updateIncomingSosLiveness() {
  unsigned long now = millis();
  bool changed = false;
  for (uint8_t i = 0; i < MAX_CANDIDATES; i++) {
    if (!incomingSos[i].inUse) continue;
    if (now - incomingSos[i].lastHeardMs > SOS_LIVENESS_TIMEOUT_MS) {
      bool wasVisible = !incomingSos[i].dismissedLocally;
      Serial.printf("[SOS] Liveness timeout for %02X:%02X:%02X:%02X:%02X:%02X\n",
                    incomingSos[i].mac[0], incomingSos[i].mac[1], incomingSos[i].mac[2],
                    incomingSos[i].mac[3], incomingSos[i].mac[4], incomingSos[i].mac[5]);
      incomingSos[i] = IncomingSos{};
      if (wasVisible) changed = true;
    }
  }
  if (changed) forceRedraw = true;
}

// Resolve a MAC to the candidate-list display name. Falls back to "??:XXXX"
// using the last two MAC bytes when the sender isn't (or isn't yet) in our
// candidate list — though the RX filter normally prevents that path.
void lookupCandidateName(const uint8_t mac[6], char* out, size_t outLen) {
  if (outLen == 0) return;
  for (uint8_t i = 0; i < candidateCount; i++) {
    if (memcmp(candidates[i].mac, mac, 6) == 0 && candidates[i].name[0]) {
      strncpy(out, candidates[i].name, outLen - 1);
      out[outLen - 1] = '\0';
      return;
    }
  }
  snprintf(out, outLen, "??:%02X%02X", mac[4], mac[5]);
}

// Pack & send one SosMessage over ESP-NOW broadcast. Used both by the 1 Hz
// active tick and by the 5-packet cancel burst on exit.
void sendSosBroadcast(bool active) {
  SosMessage m{};
  m.magic[0]    = 'S';
  m.magic[1]    = 'S';
  m.version     = 0x01;
  m.flags       = active ? 0x01 : 0x00;
  memcpy(m.senderMac, myMac, 6);
  m.seq         = mySosSeq;
  esp_now_send(broadcastAddr, (uint8_t*)&m, sizeof(m));
}

// Process an incoming SOS packet. Strict candidate-only filter: only senders
// in our group's candidate list can pop an overlay on this wearable.
//
// Marked `static` so the Arduino auto-prototype generator skips it. The
// generator inserts prototypes near the top of the file, before user types
// are declared — without `static` it would emit a forward declaration that
// references SosMessage before SosMessage exists, breaking compilation.
static void handleSosMessage(const uint8_t* srcMac, const SosMessage& m) {
  if (m.magic[0] != 'S' || m.magic[1] != 'S' || m.version != 0x01) return;
  if (!isMacInCandidates(srcMac)) {
    Serial.printf("[SOS] Ignoring out-of-group sender %02X:%02X:%02X:%02X:%02X:%02X\n",
                  srcMac[0], srcMac[1], srcMac[2], srcMac[3], srcMac[4], srcMac[5]);
    return;
  }

  unsigned long now = millis();
  int slot = -1;
  int freeSlot = -1;
  for (uint8_t i = 0; i < MAX_CANDIDATES; i++) {
    if (incomingSos[i].inUse && memcmp(incomingSos[i].mac, srcMac, 6) == 0) {
      slot = i;
      break;
    }
    if (!incomingSos[i].inUse && freeSlot < 0) freeSlot = i;
  }

  bool active = (m.flags & 0x01) != 0;
  if (active) {
    if (slot < 0) {
      if (freeSlot < 0) return;  // table full; should never happen at MAX_CANDIDATES
      slot = freeSlot;
      incomingSos[slot] = IncomingSos{};
      memcpy(incomingSos[slot].mac, srcMac, 6);
      incomingSos[slot].inUse = true;
      incomingSos[slot].lastSeq = m.seq;
      forceRedraw = true;
      Serial.printf("[SOS] *** INCOMING from %02X:%02X:%02X:%02X:%02X:%02X seq=%u ***\n",
                    srcMac[0], srcMac[1], srcMac[2], srcMac[3], srcMac[4], srcMac[5],
                    (unsigned)m.seq);
    } else {
      // Refresh existing entry. A strictly-newer seq means the sender re-armed
      // SOS after a cancel — clear any local dismissal so the wearer is alerted
      // to the new event.
      if ((int32_t)(m.seq - incomingSos[slot].lastSeq) > 0) {
        if (incomingSos[slot].dismissedLocally) forceRedraw = true;
        incomingSos[slot].dismissedLocally = false;
        incomingSos[slot].lastSeq = m.seq;
        Serial.printf("[SOS] Re-alert from %02X:%02X:%02X:%02X:%02X:%02X (new seq=%u)\n",
                      srcMac[0], srcMac[1], srcMac[2], srcMac[3], srcMac[4], srcMac[5],
                      (unsigned)m.seq);
      }
    }
    incomingSos[slot].lastHeardMs = now;
  } else {
    // Cancel: only honor for an in-flight or just-finished event with a seq
    // that matches or exceeds what we last saw. A cancel with seq < lastSeq
    // would be a stale packet for a superseded event — ignore it.
    if (slot < 0) return;
    if ((int32_t)(m.seq - incomingSos[slot].lastSeq) < 0) {
      Serial.printf("[SOS] Stale cancel ignored (got seq=%u, have=%u)\n",
                    (unsigned)m.seq, (unsigned)incomingSos[slot].lastSeq);
      return;
    }
    bool wasVisible = !incomingSos[slot].dismissedLocally;
    incomingSos[slot] = IncomingSos{};
    if (wasVisible) forceRedraw = true;
    Serial.printf("[SOS] Cancel from %02X:%02X:%02X:%02X:%02X:%02X\n",
                  srcMac[0], srcMac[1], srcMac[2], srcMac[3], srcMac[4], srcMac[5]);
  }
}

// Non-blocking 3-pulse haptic pattern: 150 ms on / 100 ms off, three times,
// then an 800 ms gap. Repeats while own SOS or any non-dismissed peer SOS is
// active. Owns MOTOR_PIN; updateHaptic() bails out while we're driving.
void updateSosHaptic() {
  static const struct { unsigned long ms; bool on; } phases[6] = {
    {150, true},  {100, false},
    {150, true},  {100, false},
    {150, true},  {800, false},
  };
  static unsigned long phaseStart = 0;
  static int8_t step = -1;  // -1 = idle (motor off, no phase running)

  bool active = sosActive || incomingSosActive();
  if (!active) {
    if (step != -1) {
      digitalWrite(MOTOR_PIN, LOW);
      step = -1;
      phaseStart = 0;
    }
    return;
  }

  unsigned long now = millis();
  if (step == -1) {
    step = 0;
    phaseStart = now;
    digitalWrite(MOTOR_PIN, phases[0].on ? HIGH : LOW);
    return;
  }
  if (now - phaseStart >= phases[step].ms) {
    step = (step + 1) % 6;
    phaseStart = now;
    digitalWrite(MOTOR_PIN, phases[step].on ? HIGH : LOW);
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
  if (len == (int)sizeof(PeerMessage)) {
    // Existing peer-location path — only accept packets from the selected target.
    if (!hasTarget) return;
    if (memcmp(info->src_addr, targetMac, 6) != 0) return;
    memcpy(&peerMsg, data, sizeof(PeerMessage));
    peerReceived = true;
    lastPeerTime = millis();
    return;
  }
  if (len == (int)sizeof(SosMessage)) {
    SosMessage m;
    memcpy(&m, data, sizeof(m));
    handleSosMessage(info->src_addr, m);
    return;
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
  // Drop any "Updated Xs ago" cache from the previous peer.
  lastValidDistance = -1;
  lastValidDistanceTime = 0;
  prevStale = false;
  prevAgoBucket = 0;
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
  // Drop any "Updated Xs ago" cache so a future re-pair starts clean.
  lastValidDistance = -1;
  lastValidDistanceTime = 0;
  prevStale = false;
  prevAgoBucket = 0;
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

// Forward declarations for the GPS-sync machinery defined later in the
// file. `pushLatchedGpsSyncResult` is static so Arduino's auto-prototype
// generator skips it; `runFocusedFixOnBoot` carries an attribute that
// the generator sometimes drops, so we declare it explicitly too.
static void pushLatchedGpsSyncResult();
[[noreturn]] void runFocusedFixOnBoot();
void triggerSyncReboot();

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
      // If we just finished a GPS sync and haven't been able to push the
      // result yet (iOS was disconnected during the sync window), fire the
      // notify now so the phone UI dismisses the in-progress overlay.
      pushLatchedGpsSyncResult();
      // Force an immediate location notify so a freshly-acquired fix lands
      // on the phone without waiting out the 15 s cadence.
      lastLocationNotify = 0;
    } else {
      Serial.println("[BLE] owner-auth FAIL — disconnecting");
      authedConn = false;
      if (pBleServer && hasActiveConn) pBleServer->disconnect(info.getConnHandle());
    }
  }
};

class GpsSyncWriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    if (!isPaired || !authedConn) {
      Serial.println("[SYNC] gps-sync-write rejected: not authed");
      return;
    }
    std::string v = c->getValue();
    if (v.size() != 1 || (uint8_t)v[0] != 0x01) {
      Serial.printf("[SYNC] gps-sync-write rejected: bad payload (%u bytes)\n",
                    (unsigned)v.size());
      return;
    }
    if (gpsSyncActive || gpsSyncTriggerPending) {
      Serial.println("[SYNC] gps-sync-write ignored: already running");
      return;
    }
    Serial.println("[SYNC] *** GPS SYNC TRIGGERED ***");
    gpsSyncTriggerPending = true;
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
static GpsSyncWriteCallbacks gpsSyncWriteCallbacks;

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
  // Clear NAV select sub-mode if it was active — the triple tap can come
  // mid-cycle, and on SOS exit we restore uiBeforeSos = NAV; we don't want
  // stale select state to reappear over the NAV view.
  navSelectActive = false;
  navSelectIndex = -1;
  navSelectConfirmLatched = false;
  navSelectConfirmFlashUntil = 0;
  navSelectSuppressShortRelease = false;
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
  // Begin SOS broadcast over ESP-NOW. seq increments on every entry so a
  // freshly-armed SOS is distinguishable from a stale cancel for the prior
  // event (defends the cancel-then-re-enter race).
  sosActive = true;
  mySosSeq++;
  sendSosBroadcast(true);
  lastSosBroadcast = millis();
  Serial.printf("[UI] Enter SOS (triple tap) seq=%u — broadcasting\n", (unsigned)mySosSeq);
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
        // Fire a flurry of cancel packets to overcome ESP-NOW packet loss so
        // every receiver's overlay clears together. Liveness timeout is the
        // safety net for any peer that misses every packet in the burst.
        for (int i = 0; i < SOS_CANCEL_BURST_COUNT; i++) {
          sendSosBroadcast(false);
          if (i < SOS_CANCEL_BURST_COUNT - 1) delay(SOS_CANCEL_BURST_GAP_MS);
        }
        sosActive = false;
        uiScreen = uiBeforeSos;
        uiShortPressCount = 0;
        uiSleepLatched = false;
        uiWakeLatched = false;
        uiFactoryLatched = false;
        uiPairFromHomeLatched = false;
        uiDeferPairingExitUntilBtnRelease = false;
        forceRedraw = true;
        Serial.println("[UI] Exit SOS (hold 3s) — cancel burst sent");
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

      // Dismiss-incoming-SOS gesture (firmware 17): a single short tap while
      // any peer SOS overlay is visible dismisses just that overlay locally.
      // The original sender is unaffected — only the wearer's own 3s-hold
      // exit propagates a cancel over ESP-NOW. Gated off while the wearer is
      // themselves in own SOS so we don't silently swallow background entries.
      if (shortPress && uiScreen != UI_SOS && incomingSosActive()) {
        int idx = latestIncomingSosIndex();
        if (idx >= 0) {
          incomingSos[idx].dismissedLocally = true;
          Serial.printf("[SOS] Local dismiss for %02X:%02X:%02X:%02X:%02X:%02X\n",
                        incomingSos[idx].mac[0], incomingSos[idx].mac[1],
                        incomingSos[idx].mac[2], incomingSos[idx].mac[3],
                        incomingSos[idx].mac[4], incomingSos[idx].mac[5]);
        }
        forceRedraw = true;
        uiShortPressCount = 0;
        uiTripleWindowStartMs = 0;
      } else
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
      } else if (shortPress && (uiScreen == UI_NAV || uiScreen == UI_HOME)) {
        // Unified short-press handler for NAV (incl. select sub-mode) and HOME.
        // Every short press counts toward the triple-tap-SOS detector with a
        // tight window (UI_TRIPLE_WINDOW_MS = 600ms). If the burst threshold
        // is hit, SOS pre-empts whatever the per-state action would have been
        // — including cycling candidates in select mode. Otherwise, fall
        // through to the per-state action (cycle / enter select / no-op).
        if (uiShortPressCount == 0 || (now - uiTripleWindowStartMs) > UI_TRIPLE_WINDOW_MS) {
          uiShortPressCount = 1;
          uiTripleWindowStartMs = now;
        } else {
          uiShortPressCount++;
        }

        if (uiShortPressCount >= 3 && (now - uiTripleWindowStartMs) <= UI_TRIPLE_WINDOW_MS) {
          armSosFromTripleTap();
        } else if (uiScreen == UI_NAV && navSelectActive) {
          // Cycle to next candidate.
          if (candidateCount > 0) {
            navSelectIndex = (navSelectIndex + 1) % (int8_t)candidateCount;
          }
          navSelectLastInteractionMs = now;
          forceRedraw = true;
          Serial.printf("[SELECT] Cycle -> %d/%u\n", (int)navSelectIndex + 1, candidateCount);
        } else if (uiScreen == UI_NAV && candidateCount > 0) {
          // Enter select mode on a quick tap when there are candidates. The
          // first tap acts as "show first candidate" so cycling N taps lands
          // on the N-th. Pre-seed to currently-tracked target if present.
          navSelectActive = true;
          navSelectIndex = 0;
          if (hasTarget) {
            for (uint8_t i = 0; i < candidateCount; i++) {
              if (memcmp(candidates[i].mac, targetMac, 6) == 0) {
                navSelectIndex = (int8_t)i;
                break;
              }
            }
          }
          navSelectLastInteractionMs = now;
          forceRedraw = true;
          Serial.printf("[SELECT] Enter -> %d/%u\n", (int)navSelectIndex + 1, candidateCount);
        }
        // HOME short tap with no candidates / not-yet-triple: no-op (matches prior behavior).
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

  // GPS sync (firmware 18). iOS writes 0x01 to request a focused fix; the
  // firmware notifies [status, secondsRemaining] back. Owner-auth gated.
  // Initial value seeded so a READ before any sync returns a sane idle state.
  pGpsSyncCharacteristic = pBleService->createCharacteristic(
    BLE_GPS_SYNC_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
  );
  pGpsSyncCharacteristic->setCallbacks(&gpsSyncWriteCallbacks);
  uint8_t initSync[2] = { GPS_SYNC_IDLE, 0 };
  pGpsSyncCharacteristic->setValue(initSync, 2);

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
  pGpsSyncCharacteristic = nullptr;
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
//  GPS SYNC (firmware 18)
// =====================================================
//
// Background:
//   When BLE + ESP-NOW + WiFi are all up, the GNSS chipset can lock onto
//   satellites (visible count climbs to 6-8) but never converges to a fix.
//   Disabling the radios — see arduino/gps_fix_tests/17_wearable_radios_off
//   for the diagnostic — restores normal acquisition behavior. The GPS-sync
//   feature is a user-initiated escape hatch: iOS asks the wearable to spend
//   30 s with all radios off, then resume normal operation with a fresh fix.
//
// State machine:
//   IDLE → (iOS writes 0x01 to abd6) → trigger pending → loop() drains it
//   → notify "running" once → flush delay → tear down radios → 30 s focused
//   GPS poll → bring radios back up → latch SUCCESS/FAILED → next owner-auth
//   pushes the latched result over abd6 (see OwnerAuthCallbacks).

// Force a redraw of the radio-off countdown screen. Called every ~500 ms
// during the focused poll loop. The countdown sits on its own screen so
// we don't have to coordinate with the normal NAV/HOME draw paths — the
// rest of `updateDisplay()` is bypassed entirely while gpsSyncActive is true.
static int gpsSyncPrevSeconds = -1;
static void drawGpsSyncScreen(int secondsRemaining) {
  if (secondsRemaining == gpsSyncPrevSeconds) return;
  gpsSyncPrevSeconds = secondsRemaining;
  tft.fillScreen(COLOR_BG);
  drawCenteredText("GPS SYNC", 50, COLOR_CYAN, 2);
  char buf[8];
  snprintf(buf, sizeof(buf), "%ds", secondsRemaining < 0 ? 0 : secondsRemaining);
  drawCenteredText(buf, 95, COLOR_WHITE, 5);
  drawCenteredText("Radios off", 170, COLOR_GRAY, 1);
  drawCenteredText("Hold the watch up", 188, COLOR_GRAY, 1);
  drawCenteredText("with a clear sky view", 200, COLOR_GRAY, 1);
}

// Result-screen flash: 1 s of green "GPS LOCKED" / red "SYNC FAILED" right
// after the focused poll completes, before normal NAV/HOME UI resumes.
static void drawGpsSyncResultScreen(bool success) {
  tft.fillScreen(success ? COLOR_BG : COLOR_RED);
  if (success) {
    drawCenteredText("GPS", 80, COLOR_GREEN, 3);
    drawCenteredText("LOCKED", 115, COLOR_GREEN, 3);
    drawCenteredText("Position synced", 170, COLOR_WHITE, 1);
  } else {
    drawCenteredText("SYNC", 80, COLOR_WHITE, 3);
    drawCenteredText("FAILED", 115, COLOR_WHITE, 3);
    drawCenteredText("No fix in 30s", 170, COLOR_WHITE, 1);
  }
}

// Push the latched sync result to iOS via abd6 notify, IFF we have a fresh
// (within the TTL) result that hasn't been delivered yet. Called from
// `OwnerAuthCallbacks::onWrite` immediately on auth, and from `loop()` to
// retry over the next ~2.5 s — BLE notifications are unreliable, especially
// right after the central has just finished service discovery and may not
// yet have actually subscribed to abd6.
#define GPS_SYNC_PUSH_REPEATS     5
#define GPS_SYNC_PUSH_INTERVAL_MS 500UL
static uint8_t       gpsSyncPushCount  = 0;
static unsigned long gpsSyncLastPushAt = 0;

static void pushLatchedGpsSyncResult() {
  if (gpsSyncResultPushed) return;
  if (gpsSyncResult == GPS_SYNC_IDLE) {
    gpsSyncResultPushed = true;
    return;
  }
  if (millis() - gpsSyncResultAt > GPS_SYNC_RESULT_TTL_MS) {
    Serial.println("[SYNC] Latched result expired — not pushing");
    gpsSyncResultPushed = true;
    gpsSyncResult = GPS_SYNC_IDLE;
    return;
  }
  if (!pGpsSyncCharacteristic) return;
  if (!hasActiveConn || !authedConn) return;
  unsigned long now = millis();
  if (gpsSyncLastPushAt != 0 && now - gpsSyncLastPushAt < GPS_SYNC_PUSH_INTERVAL_MS) return;
  uint8_t buf[2] = { gpsSyncResult, 0 };
  pGpsSyncCharacteristic->setValue(buf, 2);
  pGpsSyncCharacteristic->notify();
  gpsSyncLastPushAt = now;
  gpsSyncPushCount++;
  Serial.printf("[SYNC] Pushed latched result (%u/%u): %s\n",
                gpsSyncPushCount, GPS_SYNC_PUSH_REPEATS,
                gpsSyncResult == GPS_SYNC_SUCCESS ? "SUCCESS" : "FAILED");
  if (gpsSyncPushCount >= GPS_SYNC_PUSH_REPEATS) {
    gpsSyncResultPushed = true;
  }
}

// Persist the trigger and reboot. Called from loop() when
// gpsSyncTriggerPending is set. We notify "running" first so iOS sees the
// start before we drop off the air, then write the NVS flag and restart.
//
// In-process radio teardown (NimBLEDevice::deinit + esp_now_deinit + WiFi
// off) crashes the NimBLE host task on this build with PC=0 / EXCCAUSE=0x14
// — the disconnect that we triggered is still being processed when deinit
// frees the host's memory. Rebooting sidesteps the race entirely and gives
// the focused-fix path a pristine RF environment that exactly matches the
// `gps_fix_tests/17_wearable_radios_off` diagnostic sketch.
void triggerSyncReboot() {
  gpsSyncActive = true;
  Serial.println("[SYNC] === ENTERING SYNC (reboot path) ===");

  // Notify "running" while BLE is still up. NimBLE will queue the notify
  // PDU + the upcoming disconnect into the same connection event window;
  // a 300 ms drain is empirically enough on iOS.
  if (pGpsSyncCharacteristic) {
    uint8_t buf[2] = { GPS_SYNC_RUNNING, (uint8_t)(GPS_SYNC_DURATION_MS / 1000) };
    pGpsSyncCharacteristic->setValue(buf, 2);
    pGpsSyncCharacteristic->notify();
  }
  drawGpsSyncScreen(GPS_SYNC_DURATION_MS / 1000);
  delay(300);

  // Persist the flag and clear any stale result from a previous run.
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putBool(PREFS_KEY_SYNC_PENDING, true);
  prefs.remove(PREFS_KEY_SYNC_RESULT);
  prefs.end();
  Serial.println("[SYNC] Persisted syncpend=1, restarting...");
  delay(50);
  ESP.restart();
}

// Run when setup() detects syncpend=true in NVS. Brings up only the display
// and GPS UART — no WiFi, no ESP-NOW, no NimBLE — and reads GPS for up to
// 30 s. On exit, writes the result to NVS and reboots back into normal
// mode. This function never returns.
[[noreturn]] void runFocusedFixOnBoot() {
  Serial.println("[SYNC-BOOT] === FOCUSED FIX MODE ===");
  Serial.println("[SYNC-BOOT] Skipping all radio init; display + GPS UART only");

  // Display init (SPI; not a radio).
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(COLOR_BG);
  drawGpsSyncScreen(GPS_SYNC_DURATION_MS / 1000);

  // GPS UART init — same pins/baud as setup().
  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // Motor pin is left as input so the haptic doesn't accidentally fire
  // while we're in this stripped-down mode.
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  unsigned long syncStart = millis();
  unsigned long pollLastDisplay = 0;
  unsigned long pollLastLog = 0;
  bool gotFix = false;
  bool firstSatLogged = false;     // milestone: any constellation in view
  bool firstUsedLogged = false;    // milestone: any sat used in the fix
  unsigned long lastBytesAt = 0;
  unsigned long prevChars = 0;

  while (millis() - syncStart < GPS_SYNC_DURATION_MS) {
    while (Serial1.available()) {
      gps.encode(Serial1.read());
      lastBytesAt = millis();
    }

    unsigned long now = millis();
    unsigned long elapsed = now - syncStart;

    if (now - pollLastDisplay >= 500) {
      pollLastDisplay = now;
      int remaining = (int)((GPS_SYNC_DURATION_MS - elapsed) / 1000);
      drawGpsSyncScreen(remaining);
    }

    // Snapshot the per-constellation visibility every loop so milestone
    // logs fire as soon as the first GSV sentence arrives, not on the next
    // 1 Hz tick.
    int viewGps = parseGsvCount(gpsInView.value());
    int viewBd  = parseGsvCount(bdInView.value());
    int viewGl  = parseGsvCount(glInView.value());
    int viewGa  = parseGsvCount(gaInView.value());
    int viewTot = viewGps + viewBd + viewGl + viewGa;
    int used    = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
    bool hasFix = gps.location.isValid() && gps.location.age() < GPS_SYNC_FRESH_AGE_MS;

    if (!firstSatLogged && viewTot > 0) {
      firstSatLogged = true;
      Serial.printf("[SYNC-BOOT] >>> First satellite VISIBLE at t=%lums "
                    "(GPS=%d BD=%d GL=%d GA=%d) <<<\n",
                    elapsed, viewGps, viewBd, viewGl, viewGa);
    }
    if (!firstUsedLogged && used > 0) {
      firstUsedLogged = true;
      Serial.printf("[SYNC-BOOT] >>> First satellite USED IN FIX at t=%lums "
                    "(used=%d of %d visible) <<<\n",
                    elapsed, used, viewTot);
    }

    if (now - pollLastLog >= 1000) {
      pollLastLog = now;
      const char* state = hasFix ? "FIX" : (viewTot > 0 ? "ACQ" : "---");
      // bytes/sec from GPS UART — useful sanity check that the antenna /
      // module is actually streaming (vs. silent UART = wiring/power issue).
      unsigned long deltaChars = gps.charsProcessed() - prevChars;
      prevChars = gps.charsProcessed();
      Serial.printf("[SYNC-BOOT t=%2lus] %s  view: GPS=%d BD=%d GL=%d GA=%d "
                    "(tot %d) | used=%d",
                    elapsed / 1000, state,
                    viewGps, viewBd, viewGl, viewGa, viewTot, used);
      if (gps.hdop.isValid() && gps.hdop.hdop() < 25.0) {
        Serial.printf(" | HDOP %.1f", gps.hdop.hdop());
      }
      Serial.printf(" | rx %lub/s | age=%lums\n", deltaChars, gps.location.age());

      // Loud warning if the GPS UART isn't producing any bytes — points the
      // user at hardware (TX/RX swap, GPS not powered, antenna detached)
      // rather than letting them assume the radios-off magic isn't working.
      if (lastBytesAt == 0 && elapsed > 3000) {
        Serial.println("[SYNC-BOOT] !! No bytes from GPS UART after 3s. "
                       "Check D6/D7 wiring + GPS power.");
      }
    }

    if (hasFix) {
      gotFix = true;
      Serial.printf("[SYNC-BOOT] *** FRESH FIX at t=%lums *** "
                    "lat=%.6f lon=%.6f alt=%.1fm  used=%d/%d visible\n",
                    elapsed, gps.location.lat(), gps.location.lng(),
                    gps.altitude.isValid() ? gps.altitude.meters() : 0.0,
                    used, viewTot);
      break;
    }

    delay(10);
  }

  // Final summary line: even on FAILED, show the best visibility we saw so
  // the user can tell whether the antenna saw anything vs. silent silence.
  {
    int viewGps = parseGsvCount(gpsInView.value());
    int viewBd  = parseGsvCount(bdInView.value());
    int viewGl  = parseGsvCount(glInView.value());
    int viewGa  = parseGsvCount(gaInView.value());
    int viewTot = viewGps + viewBd + viewGl + viewGa;
    int used    = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
    Serial.printf("[SYNC-BOOT] Done after %lums — %s "
                  "(final view: GPS=%d BD=%d GL=%d GA=%d (tot %d) | used=%d | "
                  "chars rcvd=%lu)\n",
                  millis() - syncStart, gotFix ? "SUCCESS" : "FAILED",
                  viewGps, viewBd, viewGl, viewGa, viewTot, used,
                  gps.charsProcessed());
  }

  // Persist the result and clear the pending flag. The next clean boot
  // (below) reads syncres and the existing latched-result push notifies iOS.
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.remove(PREFS_KEY_SYNC_PENDING);
  prefs.putUChar(PREFS_KEY_SYNC_RESULT,
                 gotFix ? GPS_SYNC_SUCCESS : GPS_SYNC_FAILED);
  prefs.end();

  drawGpsSyncResultScreen(gotFix);
  delay(1500);

  Serial.println("[SYNC-BOOT] Restarting into normal mode");
  delay(50);
  ESP.restart();
  // Unreachable — keeps the [[noreturn]] contract honest if ESP.restart()
  // ever returns on a future SDK.
  while (true) { delay(1000); }
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

// Incoming-peer SOS overlay (firmware 17). Red "SOS" headline with the peer's
// display name underneath, resolved from the candidate list. Dismissable with
// a single short tap; the wearer's own 3s-hold exit and Power-OFF gestures
// continue to take precedence over this overlay.
static void drawIncomingSosScreen(int idx) {
  tft.fillScreen(COLOR_RED);
  drawCenteredText("SOS", 70, COLOR_WHITE, 4);

  char nameBuf[TARGET_NAME_MAX_LEN + 1];
  lookupCandidateName(incomingSos[idx].mac, nameBuf, sizeof(nameBuf));
  uint8_t nameSize = strlen(nameBuf) > 10 ? 1 : 2;
  drawCenteredText(nameBuf, 140, COLOR_WHITE, nameSize);

  drawCenteredText("Tap to dismiss", 200, COLOR_WHITE, 1);
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

  // Incoming-SOS overlay (firmware 17). Sits between own-SOS and HOME in
  // priority: any non-dismissed peer SOS pre-empts NAV / HOME / pairing UIs,
  // but the wearer's own SOS still wins. Once all peer entries are dismissed
  // or expire, the underlying screen resumes.
  if (incomingSosActive()) {
    int idx = latestIncomingSosIndex();
    if (idx >= 0 && forceRedraw) {
      drawIncomingSosScreen(idx);
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

  double liveDist = getActiveDistance();
  bool   connected = (liveDist >= 0);

  // Latch the freshest reading so we can keep displaying it after the radios
  // go silent. updateMode() leaves `currentMode` pinned at the last working
  // mode when nothing is live, so the mode chip up top remains coherent on
  // its own; we only need to remember the distance and when we got it.
  if (connected) {
    lastValidDistance     = liveDist;
    lastValidDistanceTime = millis();
  }

  // "Stale" = we don't have a live distance right now but we have one we can
  // keep showing. The screen persists the last number and replaces the
  // status line with an "Updated Xs ago" / "Updated Xm ago" counter.
  bool stale = !connected && (lastValidDistance >= 0);

  double         effectiveDist = stale ? lastValidDistance : liveDist;
  unsigned long  agoSec        = stale ? ((millis() - lastValidDistanceTime) / 1000UL) : 0;
  unsigned long  agoMin        = agoSec / 60UL;
  // Bucket changes once per second below 60s, then once per minute. Used as
  // a dirty key so the "ago" counter ticks without the surrounding fields
  // having to change.
  unsigned long  agoBucket     = (agoSec < 60UL) ? agoSec : (60UL + agoMin);

  bool phoneLinked = hasActiveConn && authedConn;

  // Fast path: if only the phone-link state changed, redraw just the icon
  // instead of the whole screen so the main UI stays flicker-free.
  bool distChanged = forceRedraw ||
    (prevDisplayDist < 0 && effectiveDist >= 0) ||
    (prevDisplayDist >= 0 && effectiveDist < 0) ||
    (effectiveDist >= 0 && fabs(effectiveDist - prevDisplayDist) > 0.1) ||
    (currentMode != prevDisplayMode) ||
    (currentBPM != prevBPM) ||
    (fingerDetected != prevFinger) ||
    (connected != prevConnected) ||
    (stale != prevStale) ||
    (stale && agoBucket != prevAgoBucket);

  if (!distChanged) {
    if (phoneLinked != prevPhoneLinked) {
      drawPhoneIcon(phoneLinked);
      prevPhoneLinked = phoneLinked;
    }
    return;
  }

  prevDisplayDist = effectiveDist;
  prevDisplayMode = currentMode;
  prevBPM = currentBPM;
  prevFinger = fingerDetected;
  prevConnected = connected;
  prevStale = stale;
  prevAgoBucket = agoBucket;
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

  // Status line: connected / "Updated Xs ago" / searching / no peer.
  // The stale branch replaces the old "NO PEER" collapse so the wearer
  // keeps seeing the last known distance plus a freshness counter.
  char        agoBuf[24];
  const char* status;
  uint16_t    statusColor;
  if (connected) {
    status = "CONNECTED";
    statusColor = COLOR_GREEN;
  } else if (stale) {
    if (agoSec < 60UL) {
      snprintf(agoBuf, sizeof(agoBuf), "Updated %lus ago", agoSec);
    } else {
      snprintf(agoBuf, sizeof(agoBuf), "Updated %lum ago", agoMin);
    }
    status = agoBuf;
    statusColor = COLOR_YELLOW;
  } else if (peerReceived || peerBleFound) {
    status = "SEARCHING...";
    statusColor = COLOR_YELLOW;
  } else {
    status = "NO PEER";
    statusColor = COLOR_RED;
  }
  drawCenteredText(status, 24, statusColor, 1);

  const char* label = getProximityLabel(effectiveDist);
  uint16_t    color = getProximityColor(effectiveDist);
  drawCenteredText(label, 38, color, 2);

  drawProximityRing(effectiveDist, color);

  char distBuf[20];
  if (effectiveDist < 0) {
    snprintf(distBuf, sizeof(distBuf), "---");
  } else if (effectiveDist < 100) {
    snprintf(distBuf, sizeof(distBuf), "%.1f m", effectiveDist);
  } else if (effectiveDist < 1000) {
    snprintf(distBuf, sizeof(distBuf), "%d m", (int)effectiveDist);
  } else {
    snprintf(distBuf, sizeof(distBuf), "%.2f km", effectiveDist / 1000.0);
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

  // Haptic indicator only when the live distance is in range — we don't
  // buzz on cached numbers, so don't lie about it on screen either.
  if (liveDist >= 0 && liveDist <= HAPTIC_RANGE_M) {
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
  Serial.println("  Spatial Wearable — Tracking UI (19)");
  Serial.println("==========================================");

  // GPS-sync re-entry check. If this boot was caused by a sync trigger
  // (loop() persisted syncpend=true and ESP.restart()ed), divert to the
  // focused-fix path BEFORE any radio init. runFocusedFixOnBoot() never
  // returns — it reboots a second time once the 30 s poll finishes.
  prefs.begin(PREFS_NAMESPACE, false);
  bool syncPendingFromNvs = prefs.getBool(PREFS_KEY_SYNC_PENDING, false);
  uint8_t syncResultFromNvs = prefs.getUChar(PREFS_KEY_SYNC_RESULT, GPS_SYNC_IDLE);
  // Clear the result key so we don't keep re-pushing it forever; it lives
  // only in RAM (gpsSyncResult) from this point on.
  if (syncResultFromNvs != GPS_SYNC_IDLE) {
    prefs.remove(PREFS_KEY_SYNC_RESULT);
  }
  prefs.end();

  if (syncPendingFromNvs) {
    Serial.println("[SETUP] syncpend=1 — entering focused-fix mode");
    runFocusedFixOnBoot();   // [[noreturn]]
  }

  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  loadPairing();
  pairingMode = false;
  uiScreen = UI_HOME;
  pairingEnteredAt = millis();

  // If the focused-fix path ran on the previous boot, surface its result.
  // The existing pushLatchedGpsSyncResult() flow notifies iOS the next time
  // the central completes owner-auth. The 60 s TTL starts ticking at boot.
  if (syncResultFromNvs == GPS_SYNC_SUCCESS || syncResultFromNvs == GPS_SYNC_FAILED) {
    gpsSyncResult       = syncResultFromNvs;
    gpsSyncResultAt     = millis();
    gpsSyncResultPushed = false;
    gpsSyncPushCount    = 0;
    gpsSyncLastPushAt   = 0;
    Serial.printf("[SETUP] Latched sync result from NVS: %s\n",
                  syncResultFromNvs == GPS_SYNC_SUCCESS ? "SUCCESS" : "FAILED");
  }

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
  // Drain a pending GPS-sync trigger before doing anything else this tick.
  // triggerSyncReboot() persists to NVS and ESP.restart()s — it does not
  // return. The next boot detects the flag in setup() and runs the
  // focused-fix poll with no radios initialized at all (avoids the NimBLE
  // host-task crash we hit when deinit()ing in-process).
  if (gpsSyncTriggerPending && !gpsSyncActive) {
    gpsSyncTriggerPending = false;
    triggerSyncReboot();
    return;
  }

  while (Serial1.available()) {
    gps.encode(Serial1.read());
  }

  sampleHeartRate();
  // Drive the SOS pattern first so it owns MOTOR_PIN; updateHaptic() bails
  // out while sosActive or any non-dismissed incoming SOS is live. Liveness
  // sweep clears stale peer entries (sender died without sending cancel).
  updateIncomingSosLiveness();
  updateSosHaptic();
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

    // Re-broadcast SOS while active so receivers refresh their liveness
    // timers. PeerMessage continues to flow alongside — the wearer's last
    // known location remains useful to anyone responding to the alert.
    if (sosActive) {
      sendSosBroadcast(true);
      lastSosBroadcast = millis();
    }

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
  // Drain a latched GPS-sync result if iOS is connected + authed. Idempotent;
  // self-rate-limits to GPS_SYNC_PUSH_INTERVAL_MS between repeat sends.
  pushLatchedGpsSyncResult();

  if (millis() - lastDisplayUpdate >= 300) {
    lastDisplayUpdate = millis();
    updateDisplay();
  }
}
