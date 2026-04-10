// ESP-NOW Two-Way Communication Test
// Flash the SAME sketch to both XIAO ESP32S3 boards.
// Auto-detects which board it's running on via MAC address.
//
// Board 1 MAC: 9C:13:9E:AD:09:B0
// Board 2 MAC: 9C:13:9E:AD:0A:2C

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

// Known board MACs
uint8_t board1_mac[] = {0x9C, 0x13, 0x9E, 0xAD, 0x09, 0xB0};
uint8_t board2_mac[] = {0x9C, 0x13, 0x9E, 0xAD, 0x0A, 0x2C};
uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

uint8_t peerMac[6];
uint8_t myBoardId = 0;

typedef struct {
  uint8_t boardId;
  uint32_t counter;
  uint32_t uptime;
} PingMessage;

uint32_t sendCounter = 0;
unsigned long lastSend = 0;
esp_now_peer_info_t peerInfo;

bool macMatch(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, 6) == 0;
}

// Native Arduino Core 3.x / ESP-IDF 5.x callback signatures (NO casts)
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.printf("[SEND] #%lu -> %s\n", (unsigned long)sendCounter,
    status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  Serial.printf("[RECV] %d bytes from %02X:%02X:%02X:%02X:%02X:%02X  RSSI: %d\n",
    len,
    info->src_addr[0], info->src_addr[1], info->src_addr[2],
    info->src_addr[3], info->src_addr[4], info->src_addr[5],
    info->rx_ctrl->rssi);
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

  WiFi.mode(WIFI_STA);

  // Read our own MAC
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);

  Serial.printf("My MAC:  %02X:%02X:%02X:%02X:%02X:%02X\n",
    myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
  Serial.printf("Channel: %d\n", WiFi.channel());

  if (macMatch(myMac, board1_mac)) {
    myBoardId = 1;
    Serial.println("Detected: BOARD 1 --> peer is Board 2");
  } else if (macMatch(myMac, board2_mac)) {
    myBoardId = 2;
    Serial.println("Detected: BOARD 2 --> peer is Board 1");
  } else {
    myBoardId = 0;
    Serial.println("WARNING: Unknown board! Using broadcast.");
  }

  // Use broadcast for now (unicast was failing)
  memcpy(peerMac, broadcastAddr, 6);
  Serial.printf("Peer:    %02X:%02X:%02X:%02X:%02X:%02X (BROADCAST)\n",
    peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5]);

  // Initialize ESP-NOW
  esp_err_t err;

  err = esp_now_init();
  Serial.printf("esp_now_init: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
  if (err != ESP_OK) { while (true) delay(1000); }

  err = esp_now_register_send_cb(onDataSent);
  Serial.printf("register_send_cb: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));

  err = esp_now_register_recv_cb(onDataRecv);
  Serial.printf("register_recv_cb: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));

  memcpy(peerInfo.peer_addr, peerMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  err = esp_now_add_peer(&peerInfo);
  Serial.printf("add_peer: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
  if (err != ESP_OK) { while (true) delay(1000); }

  Serial.println();
  Serial.println("Setup complete. Sending pings every 2 seconds...");
  Serial.println("Waiting for [RECV] lines from the other board...");
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
    if (result != ESP_OK) {
      Serial.printf("[SEND] esp_now_send error: %s\n", esp_err_to_name(result));
    }
  }
}
