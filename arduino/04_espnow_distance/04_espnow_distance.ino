// ESP-NOW Distance Calculator
// Two XIAO ESP32S3 boards, each with a GPS module, broadcast their
// position to each other and calculate the distance between them.
//
// ============================================================
//  SETUP FOR TWO BOARDS
// ============================================================
//
//  By default this sketch uses BROADCAST mode (FF:FF:FF:FF:FF:FF).
//  Both boards run the EXACT SAME code -- no changes needed.
//  Just flash this sketch to both, wire up GPS on each, and go.
//
//  HOW IT WORKS:
//    - Each board reads its own GPS position
//    - Every ~1 second it broadcasts its lat/lng over ESP-NOW
//    - When it receives the other board's position, it calculates
//      the distance using the Haversine formula and prints it
//
//  WIRING (same on both boards):
//    GPS TXD --> D0 (GPIO1)
//    GPS RXD --> D1 (GPIO2)
//    GPS VCC --> 3V3
//    GPS GND --> GND
//
//  OPTIONAL -- UNICAST MODE (more reliable, delivery confirmation):
//    1. Flash both boards and open serial monitors
//    2. Each board prints its MAC address at startup
//    3. Replace broadcastAddress below with the OTHER board's MAC
//    4. Re-flash each board with its peer's MAC address
//
// ============================================================

#include <WiFi.h>
#include <esp_now.h>
#include <TinyGPSPlus.h>

// GPS pin config (same as 02_gps_test Option A)
#define GPS_RX_PIN 1   // D0 - GPIO1 - connects to GPS TXD
#define GPS_TX_PIN 2   // D1 - GPIO2 - connects to GPS RXD

// Broadcast address -- works without knowing the peer's MAC.
// For unicast, replace with the other board's MAC (printed at boot).
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Message structure sent over ESP-NOW
typedef struct {
  float lat;
  float lng;
  bool valid;
} GpsMessage;

TinyGPSPlus gps;
GpsMessage myData;
GpsMessage peerData;
bool peerReceived = false;
unsigned long lastSend = 0;
unsigned long lastPeerTime = 0;

// Earth radius in meters
const double EARTH_RADIUS = 6371000.0;

// --- Haversine formula ---
// Returns distance in meters between two lat/lng points.
// This accounts for Earth's curvature and is the standard
// formula for GPS coordinate distance calculation.
double haversine(double lat1, double lon1, double lat2, double lon2) {
  double dLat = radians(lat2 - lat1);
  double dLon = radians(lon2 - lon1);
  double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
             cos(radians(lat1)) * cos(radians(lat2)) *
             sin(dLon / 2.0) * sin(dLon / 2.0);
  double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
  return EARTH_RADIUS * c;
}

// ESP-NOW send callback (v3.x API: first arg is wifi_tx_info_t*)
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.printf("[ESP-NOW] Send: %s\n", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ESP-NOW receive callback
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  Serial.printf("[ESP-NOW] Recv %d bytes from %02X:%02X:%02X:%02X:%02X:%02X\n",
    len, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  if (len == sizeof(GpsMessage)) {
    memcpy(&peerData, data, sizeof(GpsMessage));
    peerReceived = true;
    lastPeerTime = millis();
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("==========================================");
  Serial.println("  ESP-NOW GPS Distance Calculator");
  Serial.println("==========================================");

  // Init WiFi radio (needed for ESP-NOW, but does NOT connect to any network)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();  // ensure not trying to connect to anything
  delay(500);         // give radio time to start

  Serial.printf("WiFi channel: %d\n", WiFi.channel());
  Serial.printf("This board's MAC: %s\n", WiFi.macAddress().c_str());

  // Sanity check -- if MAC is all zeros, radio didn't start
  if (WiFi.macAddress() == "00:00:00:00:00:00") {
    Serial.println("WARNING: MAC is all zeros! Radio may not have initialized.");
    Serial.println("Try: power cycle the board, or check if WiFi is supported.");
  }
  Serial.println();
  Serial.println("Wiring (same on both boards):");
  Serial.println("  GPS TXD --> D0 (GPIO1)");
  Serial.println("  GPS RXD --> D1 (GPIO2)");
  Serial.println("  GPS VCC --> 3V3");
  Serial.println("  GPS GND --> GND");
  Serial.println();

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: ESP-NOW init failed!");
    while (true) delay(1000);
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  // Add broadcast peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;  // use current channel
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("ERROR: Failed to add peer!");
    while (true) delay(1000);
  }

  Serial.println("ESP-NOW initialized. Waiting for GPS fix...");
  Serial.println("==========================================");
  Serial.println();

  // Init GPS UART
  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
}

void loop() {
  // Feed GPS parser
  while (Serial1.available()) {
    gps.encode(Serial1.read());
  }

  // Send our position every ~1 second
  if (millis() - lastSend >= 1000) {
    lastSend = millis();

    myData.valid = gps.location.isValid();
    if (myData.valid) {
      myData.lat = gps.location.lat();
      myData.lng = gps.location.lng();
    } else {
      myData.lat = 0;
      myData.lng = 0;
    }

    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&myData, sizeof(myData));
    if (result != ESP_OK) {
      Serial.printf("[ESP-NOW] Send error: %s\n", esp_err_to_name(result));
    }

    // Print status
    Serial.println("--- Status ---");
    Serial.printf("Sats: %d  |  ", gps.satellites.isValid() ? gps.satellites.value() : 0);

    if (myData.valid) {
      Serial.printf("My position:   %.6f, %.6f\n", myData.lat, myData.lng);
    } else {
      Serial.println("My position:   waiting for fix...");
    }

    // Check if peer data is stale (>5 seconds old)
    bool peerStale = (millis() - lastPeerTime > 5000);

    if (peerReceived && !peerStale) {
      if (peerData.valid) {
        Serial.printf("Peer position: %.6f, %.6f\n", peerData.lat, peerData.lng);

        if (myData.valid) {
          double dist = haversine(myData.lat, myData.lng, peerData.lat, peerData.lng);

          Serial.printf(">> DISTANCE: %.1f meters", dist);
          if (dist >= 1000) {
            Serial.printf(" (%.2f km)", dist / 1000.0);
          }
          Serial.println();

          // Also show raw coordinate deltas for debugging
          double dLat = peerData.lat - myData.lat;
          double dLng = peerData.lng - myData.lng;
          Serial.printf("   delta lat: %+.6f  delta lng: %+.6f\n", dLat, dLng);
        } else {
          Serial.println(">> Waiting for local GPS fix to calculate distance...");
        }
      } else {
        Serial.println("Peer position: peer has no fix yet");
      }
    } else if (peerReceived && peerStale) {
      Serial.println("Peer position: STALE (no data in 5s)");
    } else {
      Serial.println("Peer position: no peer detected yet");
    }

    Serial.println();
  }
}
