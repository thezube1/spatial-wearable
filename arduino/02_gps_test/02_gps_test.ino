// GPS-only isolation test for XIAO ESP32S3
// No SPI, no display -- just UART to GPS module
// Purpose: rule out pin conflicts with SPI/display

#include <TinyGPSPlus.h>

// Try different RX/TX pins if one set doesn't work.
// Uncomment ONE pair at a time:

// Option A: D0/D1 (GPIO1/GPIO2) -- avoids SPI pins entirely
#define GPS_RX_PIN 1   // D0 - GPIO1 - connects to GPS TXD
#define GPS_TX_PIN 2   // D1 - GPIO2 - connects to GPS RXD

// Option B: D6/D7 (GPIO43/GPIO44) -- dedicated UART pins
// #define GPS_RX_PIN 44  // D7 - GPIO44 - connects to GPS TXD
// #define GPS_TX_PIN 43  // D6 - GPIO43 - connects to GPS RXD

// Option C: original pins from display_test sketch
// #define GPS_RX_PIN 8   // D9 - GPIO8 - connects to GPS TXD
// #define GPS_TX_PIN 1   // D0 - GPIO1 - connects to GPS RXD

TinyGPSPlus gps;

unsigned long startTime = 0;
unsigned long lastStatus = 0;

void setup() {
  Serial.begin(115200);
  delay(2000);  // extra time for serial monitor to connect

  Serial.println("=================================");
  Serial.println("  GPS Isolation Test");
  Serial.println("  No display, no SPI");
  Serial.println("=================================");
  Serial.printf("GPS RX pin (ESP32 input):  GPIO%d\n", GPS_RX_PIN);
  Serial.printf("GPS TX pin (ESP32 output): GPIO%d\n", GPS_TX_PIN);
  Serial.println();
  Serial.println("Wiring check:");
  Serial.println("  GPS TXD --> ESP32 RX pin above");
  Serial.println("  GPS RXD --> ESP32 TX pin above");
  Serial.println("  GPS VCC --> 3V3");
  Serial.println("  GPS GND --> GND");
  Serial.println();
  Serial.println("Phase 1: Raw byte dump (60s)");
  Serial.println("  If you see $GNGGA/$GNRMC lines, UART works.");
  Serial.println("  If you see garbage, try 4800 baud instead.");
  Serial.println("  If you see nothing, check wiring/pin swap.");
  Serial.println("=================================");
  Serial.println();

  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  startTime = millis();
  lastStatus = millis();
}

void loop() {
  unsigned long elapsed = millis() - startTime;

  // Phase 1: Raw NMEA dump for first 60 seconds
  if (elapsed < 60000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      Serial.write(c);  // pass through raw bytes
      gps.encode(c);
    }

    // Print status every 10 seconds
    if (millis() - lastStatus > 10000) {
      lastStatus = millis();
      Serial.println();
      Serial.printf("[%lus] Chars: %lu  Sentences OK: %lu  Failed: %lu  Sats: %d\n",
        elapsed / 1000,
        gps.charsProcessed(),
        gps.passedChecksum(),
        gps.failedChecksum(),
        gps.satellites.isValid() ? gps.satellites.value() : 0);

      if (gps.charsProcessed() == 0) {
        Serial.println("  >>> NO DATA received! Check wiring or try different pins.");
      } else if (gps.passedChecksum() == 0 && gps.failedChecksum() > 0) {
        Serial.println("  >>> Getting data but checksums failing -- try 4800 baud?");
      }
      Serial.println();
    }
  }
  // Phase 2: Parsed output after 60 seconds
  else {
    while (Serial1.available()) {
      gps.encode(Serial1.read());
    }

    if (millis() - lastStatus > 2000) {
      lastStatus = millis();

      Serial.println("--- GPS Status ---");
      Serial.printf("Chars: %lu  Pass: %lu  Fail: %lu\n",
        gps.charsProcessed(), gps.passedChecksum(), gps.failedChecksum());

      if (gps.location.isValid()) {
        Serial.printf("FIX!  Lat: %.6f  Lng: %.6f\n",
          gps.location.lat(), gps.location.lng());
      } else {
        Serial.printf("No fix.  Sats: %d\n",
          gps.satellites.isValid() ? gps.satellites.value() : 0);
      }

      if (gps.date.isValid() && gps.time.isValid()) {
        Serial.printf("Date: %04d-%02d-%02d  Time: %02d:%02d:%02d UTC\n",
          gps.date.year(), gps.date.month(), gps.date.day(),
          gps.time.hour(), gps.time.minute(), gps.time.second());
      }
      Serial.println();
    }
  }
}
