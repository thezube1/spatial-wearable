// ESP-NOW Two-Way Data Exchange
// Flash the SAME sketch to both XIAO ESP32S3 boards.
// Each board sends sample sensor data to the other and displays
// what it receives. Auto-detects which board via MAC address.
//
// Based on: https://randomnerdtutorials.com/esp-now-two-way-communication-esp32/
//
// Board 1 MAC: 9C:13:9E:AD:09:B0
// Board 2 MAC: 9C:13:9E:AD:0A:2C
//
// ============================================================
//  HOW IT WORKS
// ============================================================
//  - Each board generates sample sensor readings (temperature,
//    humidity, pressure) using randomized base values
//  - Every 2 seconds it broadcasts the data over ESP-NOW
//  - When it receives the other board's data, it prints it
//  - Both boards run identical code -- no changes needed
// ============================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// --- Known board MACs ---
uint8_t board1_mac[] = {0x9C, 0x13, 0x9E, 0xAD, 0x09, 0xB0};
uint8_t board2_mac[] = {0x9C, 0x13, 0x9E, 0xAD, 0x0A, 0x2C};
uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// --- Data structure sent over ESP-NOW (must match on both boards) ---
typedef struct {
  uint8_t boardId;      // which board sent this
  uint32_t msgCount;    // message sequence number
  float temperature;    // degrees C
  float humidity;       // percent
  float pressure;       // hPa
  uint32_t uptime;      // sender uptime in ms
} SensorMessage;

// Outgoing and incoming data
SensorMessage outgoing;
SensorMessage incoming;

// State
uint8_t myBoardId = 0;
uint32_t sendCount = 0;
unsigned long lastSendTime = 0;
unsigned long lastRecvTime = 0;
bool peerReceived = false;
String lastSendStatus = "";

// --- Helpers ---

bool macMatch(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, 6) == 0;
}

// Generate sample sensor values with small random drift
float sampleTemperature(uint8_t board) {
  float base = (board == 1) ? 23.5 : 25.0;
  return base + random(-20, 21) / 10.0;  // +/- 2.0 C
}

float sampleHumidity(uint8_t board) {
  float base = (board == 1) ? 55.0 : 60.0;
  return base + random(-50, 51) / 10.0;  // +/- 5.0 %
}

float samplePressure(uint8_t board) {
  float base = (board == 1) ? 1013.25 : 1012.50;
  return base + random(-30, 31) / 10.0;  // +/- 3.0 hPa
}

// --- ESP-NOW callbacks (v3.x / ESP-IDF 5.x signatures) ---

void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  lastSendStatus = (status == ESP_NOW_SEND_SUCCESS) ? "OK" : "FAIL";
  Serial.printf("[SEND] #%lu -> %s\n", (unsigned long)sendCount, lastSendStatus.c_str());
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  Serial.printf("[RECV] %d bytes from %02X:%02X:%02X:%02X:%02X:%02X  RSSI: %d\n",
    len,
    info->src_addr[0], info->src_addr[1], info->src_addr[2],
    info->src_addr[3], info->src_addr[4], info->src_addr[5],
    info->rx_ctrl->rssi);

  if (len == sizeof(SensorMessage)) {
    memcpy(&incoming, data, sizeof(SensorMessage));
    peerReceived = true;
    lastRecvTime = millis();

    Serial.println("  --- Incoming Data ---");
    Serial.printf("  Board:       %d\n", incoming.boardId);
    Serial.printf("  Message #:   %lu\n", (unsigned long)incoming.msgCount);
    Serial.printf("  Temperature: %.1f C\n", incoming.temperature);
    Serial.printf("  Humidity:    %.1f %%\n", incoming.humidity);
    Serial.printf("  Pressure:    %.1f hPa\n", incoming.pressure);
    Serial.printf("  Peer uptime: %lu ms\n", (unsigned long)incoming.uptime);
    Serial.println();
  } else {
    Serial.printf("  WARNING: unexpected payload size %d (expected %d)\n",
      len, sizeof(SensorMessage));
  }
}

// --- Setup ---

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("==========================================");
  Serial.println("  ESP-NOW Two-Way Data Exchange");
  Serial.println("==========================================");

  // Init WiFi radio (needed for ESP-NOW, does NOT connect to a network)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(500);

  // Lock both boards to channel 1
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  // Read our own MAC and auto-detect board identity
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);

  Serial.printf("My MAC:    %02X:%02X:%02X:%02X:%02X:%02X\n",
    myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
  Serial.printf("Channel:   %d\n", WiFi.channel());

  if (macMatch(myMac, board1_mac)) {
    myBoardId = 1;
    Serial.println("Detected:  BOARD 1 --> peer is Board 2");
  } else if (macMatch(myMac, board2_mac)) {
    myBoardId = 2;
    Serial.println("Detected:  BOARD 2 --> peer is Board 1");
  } else {
    myBoardId = 0;
    Serial.println("WARNING:   Unknown board! Using broadcast with ID 0.");
  }

  // Seed random number generator for sample data variation
  randomSeed(analogRead(0) ^ millis());

  // Initialize ESP-NOW
  esp_err_t err;

  err = esp_now_init();
  Serial.printf("esp_now_init:       %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
  if (err != ESP_OK) { while (true) delay(1000); }

  err = esp_now_register_send_cb(onDataSent);
  Serial.printf("register_send_cb:   %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));

  err = esp_now_register_recv_cb(onDataRecv);
  Serial.printf("register_recv_cb:   %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));

  // Add broadcast peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddr, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;

  err = esp_now_add_peer(&peerInfo);
  Serial.printf("add_peer:           %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
  if (err != ESP_OK) { while (true) delay(1000); }

  Serial.println();
  Serial.println("Setup complete. Exchanging data every 2 seconds...");
  Serial.println("==========================================");
  Serial.println();
}

// --- Main loop ---

void loop() {
  if (millis() - lastSendTime >= 2000) {
    lastSendTime = millis();
    sendCount++;

    // Build outgoing message with sample data
    outgoing.boardId = myBoardId;
    outgoing.msgCount = sendCount;
    outgoing.temperature = sampleTemperature(myBoardId);
    outgoing.humidity = sampleHumidity(myBoardId);
    outgoing.pressure = samplePressure(myBoardId);
    outgoing.uptime = millis();

    // Print what we're sending
    Serial.println("--- Sending ---");
    Serial.printf("  Board:       %d\n", outgoing.boardId);
    Serial.printf("  Message #:   %lu\n", (unsigned long)outgoing.msgCount);
    Serial.printf("  Temperature: %.1f C\n", outgoing.temperature);
    Serial.printf("  Humidity:    %.1f %%\n", outgoing.humidity);
    Serial.printf("  Pressure:    %.1f hPa\n", outgoing.pressure);

    esp_err_t result = esp_now_send(broadcastAddr, (uint8_t *)&outgoing, sizeof(outgoing));
    if (result != ESP_OK) {
      Serial.printf("[SEND] esp_now_send error: %s\n", esp_err_to_name(result));
    }

    // Print peer status
    bool peerStale = (millis() - lastRecvTime > 5000);
    if (peerReceived && !peerStale) {
      Serial.printf("  Last from peer (Board %d): T=%.1f C  H=%.1f %%  P=%.1f hPa\n",
        incoming.boardId, incoming.temperature, incoming.humidity, incoming.pressure);
    } else if (peerReceived && peerStale) {
      Serial.println("  Peer: STALE (no data in 5s)");
    } else {
      Serial.println("  Peer: no data received yet");
    }
    Serial.println();
  }
}
