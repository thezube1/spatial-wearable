// ESP-NOW Two-Way Communication Test
// Flash the SAME sketch to both XIAO ESP32S3 boards.
// Auto-detects which board it's running on via MAC address,
// then exchanges pings with the other board.
//
// Board 1 MAC: 9C:13:9E:AD:09:B0
// Board 2 MAC: 9C:13:9E:AD:0A:2C
//
// Set USE_BROADCAST to true to test basic radio connectivity first.
// Once broadcast works, set to false for unicast (delivery confirmation).

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

// ---- CONFIG ----
// Set to true to use broadcast (easier debugging), false for unicast
#define USE_BROADCAST true

// Known board MACs
uint8_t board1_mac[] = {0x9C, 0x13, 0x9E, 0xAD, 0x09, 0xB0};
uint8_t board2_mac[] = {0x9C, 0x13, 0x9E, 0xAD, 0x0A, 0x2C};
uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Peer address (set at boot)
uint8_t peerMac[6];
uint8_t myBoardId = 0;  // 1 or 2, 0 = unknown

// Ping message -- identical struct on both boards
typedef struct {
  uint8_t boardId;
  uint32_t counter;
  uint32_t uptime;
} PingMessage;

uint32_t sendCounter = 0;
unsigned long lastSend = 0;

// Peer info as global (matches tutorial pattern -- globals are zero-initialized)
esp_now_peer_info_t peerInfo;

bool macMatch(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, 6) == 0;
}

// Callbacks: simple signatures + cast at registration (tutorial pattern)
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.printf("[SEND] #%lu -> %s\n", (unsigned long)sendCounter,
    status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  Serial.printf("[RECV] %d bytes from %02X:%02X:%02X:%02X:%02X:%02X\n",
    len, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  if (len == sizeof(PingMessage)) {
    PingMessage msg;
    memcpy(&msg, data, sizeof(msg));
    Serial.printf("       Board %d | ping #%lu | uptime %lu ms\n",
      msg.boardId, (unsigned long)msg.counter, (unsigned long)msg.uptime);
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("==========================================");
  Serial.println("  ESP-NOW Two-Way Communication Test");
  Serial.println("==========================================");

  // WiFi radio in station mode (required for ESP-NOW)
  WiFi.mode(WIFI_STA);

  // Read our own MAC
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);

  Serial.printf("My MAC:  %02X:%02X:%02X:%02X:%02X:%02X\n",
    myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
  Serial.printf("Channel: %d\n", WiFi.channel());

  // Detect which board we are and set peer
  if (macMatch(myMac, board1_mac)) {
    myBoardId = 1;
    memcpy(peerMac, USE_BROADCAST ? broadcastAddr : board2_mac, 6);
    Serial.println("Detected: BOARD 1 --> peer is Board 2");
  } else if (macMatch(myMac, board2_mac)) {
    myBoardId = 2;
    memcpy(peerMac, USE_BROADCAST ? broadcastAddr : board1_mac, 6);
    Serial.println("Detected: BOARD 2 --> peer is Board 1");
  } else {
    myBoardId = 0;
    memcpy(peerMac, broadcastAddr, 6);
    Serial.println("WARNING: MAC does not match either known board!");
    Serial.println("         Using broadcast as fallback.");
  }

  Serial.printf("Peer:    %02X:%02X:%02X:%02X:%02X:%02X (%s)\n",
    peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5],
    USE_BROADCAST ? "BROADCAST" : "UNICAST");

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: esp_now_init() failed!");
    while (true) delay(1000);
  }

  // Register send callback
  esp_now_register_send_cb(esp_now_send_cb_t(OnDataSent));

  // Add peer (tutorial order: send_cb -> add_peer -> recv_cb)
  memcpy(peerInfo.peer_addr, peerMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("ERROR: esp_now_add_peer() failed!");
    while (true) delay(1000);
  }

  // Register receive callback
  esp_now_register_recv_cb(esp_now_recv_cb_t(OnDataRecv));

  Serial.println();
  Serial.println("Setup complete. Sending pings every 2 seconds...");
  Serial.println("==========================================");
  Serial.println();
}

void loop() {
  if (millis() - lastSend >= 2000) {
    lastSend = millis();
    sendCounter++;

    PingMessage msg;
    msg.boardId = myBoardId;
    msg.counter = sendCounter;
    msg.uptime = millis();

    esp_err_t result = esp_now_send(peerMac, (uint8_t *)&msg, sizeof(msg));
    if (result == ESP_OK) {
      Serial.println("Sent with success");
    } else {
      Serial.printf("[SEND] esp_now_send error: %s\n", esp_err_to_name(result));
    }
  }
}
