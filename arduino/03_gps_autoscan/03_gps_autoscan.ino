// GPS Auto-Scan: tries every common baud rate on a single pin
// Only ONE wire needed: GPS module TXD --> ESP32 test pin
//
// Instructions:
//   1. Connect GPS VCC --> 3V3
//   2. Connect GPS GND --> GND
//   3. Connect GPS TXD --> D0 (GPIO1)
//   4. If nothing found, move that SAME wire to the pin labeled RXD on
//      the GPS module (some boards have swapped labels)
//   5. Open serial monitor at 115200

#define TEST_PIN 1  // D0 / GPIO1 -- change if needed

const long baudRates[] = {4800, 9600, 19200, 38400, 57600, 115200};
const int numBauds = sizeof(baudRates) / sizeof(baudRates[0]);

void setup() {
  Serial.begin(115200);
  delay(3000);

  Serial.println("==========================================");
  Serial.println("  GPS UART Auto-Scanner");
  Serial.println("==========================================");
  Serial.printf("Test pin: GPIO%d (D0)\n", TEST_PIN);
  Serial.println();
  Serial.println("Only ONE wire needed from GPS to ESP32:");
  Serial.println("  GPS TXD --> GPIO1 (D0)");
  Serial.println("  GPS VCC --> 3V3");
  Serial.println("  GPS GND --> GND");
  Serial.println();
  Serial.println("If nothing found on TXD, try moving the");
  Serial.println("wire to the GPS pin labeled RXD instead.");
  Serial.println("(Some modules have swapped labels.)");
  Serial.println("==========================================");
  Serial.println();
}

void loop() {
  for (int i = 0; i < numBauds; i++) {
    long baud = baudRates[i];
    Serial.printf("Trying %ld baud... ", baud);

    Serial1.begin(baud, SERIAL_8N1, TEST_PIN, -1);  // RX only, no TX needed
    delay(100);

    // Flush any stale bytes
    while (Serial1.available()) Serial1.read();

    // Listen for 3 seconds
    unsigned long start = millis();
    int byteCount = 0;
    int printableCount = 0;
    bool foundDollar = false;
    char buf[256];
    int bufIdx = 0;

    while (millis() - start < 3000) {
      if (Serial1.available()) {
        char c = Serial1.read();
        byteCount++;
        if (c >= 0x20 && c <= 0x7E) printableCount++;
        if (c == '$') foundDollar = true;
        if (bufIdx < 255) buf[bufIdx++] = c;
      }
    }
    buf[bufIdx] = '\0';

    Serial1.end();

    if (byteCount == 0) {
      Serial.println("no data");
    } else if (foundDollar && printableCount > byteCount / 2) {
      // Found NMEA!
      Serial.printf("FOUND IT! %d bytes, NMEA detected\n", byteCount);
      Serial.println();
      Serial.println("========== SUCCESS ==========");
      Serial.printf("Baud rate: %ld\n", baud);
      Serial.printf("Pin: GPIO%d\n", TEST_PIN);
      Serial.println("First bytes received:");
      Serial.println("----");
      Serial.println(buf);
      Serial.println("----");
      Serial.println("=============================");
      Serial.println();

      // Now stream continuously
      Serial.println("Streaming GPS data:");
      Serial.println();
      Serial1.begin(baud, SERIAL_8N1, TEST_PIN, -1);
      while (true) {
        if (Serial1.available()) {
          Serial.write(Serial1.read());
        }
      }
    } else {
      // Got bytes but not NMEA -- probably wrong baud
      Serial.printf("got %d bytes but not NMEA (garbage/wrong baud)\n", byteCount);
      Serial.print("  Raw hex: ");
      for (int j = 0; j < min(byteCount, 20); j++) {
        Serial.printf("%02X ", (uint8_t)buf[j]);
      }
      Serial.println();
    }
  }

  Serial.println();
  Serial.println("--- No NMEA found at any baud rate ---");
  Serial.println("Things to try:");
  Serial.println("  1. Move wire from GPS TXD to GPS RXD (labels may be swapped)");
  Serial.println("  2. Try a different ESP32 pin (edit TEST_PIN)");
  Serial.println("  3. Check GPS module has power (LED blinking?)");
  Serial.println("  4. Verify GND is shared between GPS and ESP32");
  Serial.println();
  Serial.println("Restarting scan in 10 seconds...");
  Serial.println();
  delay(10000);
}
