#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <TinyGPSPlus.h>

// Display pin definitions for XIAO ESP32S3
#define TFT_SCK   7   // D8 - GPIO7
#define TFT_MOSI  9   // D10 - GPIO9
#define TFT_CS    2   // D1 - GPIO2
#define TFT_DC    3   // D2 - GPIO3
#define TFT_RST  -1   // RST tied to 3.3V

// GPS UART pins
#define GPS_RX_PIN 44  // D7 - connects to GPS TXD
#define GPS_TX_PIN 43  // D6 - connects to GPS RXD

Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);
TinyGPSPlus gps;

// Track previous values to only redraw on change
double prevLat = 0.0;
double prevLng = 0.0;
int prevSats = -1;
bool prevFix = false;

void drawLabel(const char* text, int y, uint16_t color) {
  tft.setTextColor(color, GC9A01A_BLACK);
  tft.setTextSize(2);
  tft.setCursor(30, y);
  tft.print(text);
}

void drawValue(const char* value, int y) {
  tft.setTextColor(GC9A01A_WHITE, GC9A01A_BLACK);
  tft.setTextSize(2);
  tft.setCursor(30, y);
  // Pad with spaces to clear old text
  char buf[20];
  snprintf(buf, sizeof(buf), "%-16s", value);
  tft.print(buf);
}

void drawDisplay() {
  bool hasFix = gps.location.isValid();
  int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  double lat = gps.location.lat();
  double lng = gps.location.lng();

  // Skip redraw if nothing changed
  if (hasFix == prevFix && sats == prevSats && lat == prevLat && lng == prevLng) {
    return;
  }
  prevFix = hasFix;
  prevSats = sats;
  prevLat = lat;
  prevLng = lng;

  // Header
  drawLabel("GPS + DISPLAY", 20, GC9A01A_CYAN);

  // Satellites
  char satBuf[20];
  snprintf(satBuf, sizeof(satBuf), "Sats: %d", sats);
  drawValue(satBuf, 60);

  // Fix status
  if (hasFix) {
    drawValue("Fix: YES", 85);

    // Latitude
    drawLabel("Latitude:", 115, GC9A01A_GREEN);
    char latBuf[20];
    dtostrf(lat, 10, 6, latBuf);
    drawValue(latBuf, 140);

    // Longitude
    drawLabel("Longitude:", 170, GC9A01A_GREEN);
    char lngBuf[20];
    dtostrf(lng, 10, 6, lngBuf);
    drawValue(lngBuf, 195);
  } else {
    drawValue("Fix: NO", 85);
    drawLabel("Waiting for", 120, GC9A01A_YELLOW);
    drawLabel("satellites...", 145, GC9A01A_YELLOW);

    // Clear lat/lng area
    drawValue("", 170);
    drawValue("", 195);
  }
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  delay(1000);

  Serial.println("GPS + Display Test");

  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(GC9A01A_BLACK);

  drawLabel("GPS + DISPLAY", 20, GC9A01A_CYAN);
  drawLabel("Initializing...", 120, GC9A01A_YELLOW);
}

void loop() {
  // Feed GPS data
  while (Serial1.available()) {
    gps.encode(Serial1.read());
  }

  // Update display every 500ms
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 500) {
    lastUpdate = millis();
    drawDisplay();

    // Debug to serial
    if (gps.location.isValid()) {
      Serial.printf("Lat: %.6f  Lng: %.6f  Sats: %d\n",
        gps.location.lat(), gps.location.lng(), gps.satellites.value());
    } else {
      Serial.printf("No fix yet. Sats: %d  Chars: %lu\n",
        gps.satellites.isValid() ? gps.satellites.value() : 0,
        gps.charsProcessed());
    }
  }
}
