// GPS-only isolation test for XIAO ESP32S3
// No SPI, no display -- just UART to GPS module
// Pretty-printed status so you can watch satellites come in.

#include <TinyGPSPlus.h>

// Pins match sketch 14 (D7=RX, D6=TX)
#define GPS_RX_PIN 44  // D7 - GPIO44 - connects to GPS TXD
#define GPS_TX_PIN 43  // D6 - GPIO43 - connects to GPS RXD

TinyGPSPlus gps;

// Custom parsers for "satellites in view" per constellation.
// GSV sentence field 3 (0-indexed) = total sats in view for that talker.
TinyGPSCustom gpsInView(gps, "GPGSV", 3);  // GPS
TinyGPSCustom bdInView(gps, "BDGSV", 3);   // BeiDou
TinyGPSCustom glInView(gps, "GLGSV", 3);   // GLONASS
TinyGPSCustom gaInView(gps, "GAGSV", 3);   // Galileo

unsigned long startTime = 0;
unsigned long lastStatus = 0;

bool firstSatLogged = false;
bool firstFixLogged = false;

int parseInt(const char* s) {
  if (!s || !*s) return 0;
  return atoi(s);
}

void printHeader() {
  Serial.println();
  Serial.println("=======================================");
  Serial.println("        GPS Isolation Test");
  Serial.println("=======================================");
  Serial.printf(" RX (ESP32 in):  GPIO%d  <-- GPS TXD\n", GPS_RX_PIN);
  Serial.printf(" TX (ESP32 out): GPIO%d  --> GPS RXD\n", GPS_TX_PIN);
  Serial.println(" Baud: 9600");
  Serial.println("---------------------------------------");
  Serial.println(" Watching for satellites...");
  Serial.println(" (place antenna near a window/outside)");
  Serial.println("=======================================");
  Serial.println();
}

void printStatus() {
  unsigned long elapsed = (millis() - startTime) / 1000;

  int viewGps = parseInt(gpsInView.value());
  int viewBd  = parseInt(bdInView.value());
  int viewGl  = parseInt(glInView.value());
  int viewGa  = parseInt(gaInView.value());
  int viewTotal = viewGps + viewBd + viewGl + viewGa;

  int usedSats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  bool hasFix  = gps.location.isValid() && gps.location.age() < 5000;

  // Milestone events
  if (viewTotal > 0 && !firstSatLogged) {
    firstSatLogged = true;
    Serial.println();
    Serial.printf(" >>> First satellite visible at t=%lus <<<\n", elapsed);
    Serial.println();
  }
  if (hasFix && !firstFixLogged) {
    firstFixLogged = true;
    Serial.println();
    Serial.printf(" *** FIRST FIX acquired at t=%lus ***\n", elapsed);
    Serial.println();
  }

  Serial.printf("[t=%4lus] ", elapsed);
  if (hasFix) {
    Serial.print("FIX  ");
  } else if (viewTotal > 0) {
    Serial.print("ACQ  ");
  } else {
    Serial.print("---  ");
  }

  // Satellites: in view per constellation, and used in fix
  Serial.printf("view: GPS=%-2d BD=%-2d GL=%-2d GA=%-2d (tot %2d) | used=%-2d",
                viewGps, viewBd, viewGl, viewGa, viewTotal, usedSats);

  if (gps.hdop.isValid() && gps.hdop.hdop() < 25.0) {
    Serial.printf(" | HDOP %.1f", gps.hdop.hdop());
  }

  Serial.println();

  // Position line (only when we have / had a fix)
  if (hasFix) {
    Serial.printf("           lat %.6f  lng %.6f  alt %.1fm  spd %.1f km/h\n",
                  gps.location.lat(),
                  gps.location.lng(),
                  gps.altitude.isValid() ? gps.altitude.meters() : 0.0,
                  gps.speed.isValid()    ? gps.speed.kmph()      : 0.0);
  }

  // UTC time line (valid even before fix once GPS sees one satellite)
  if (gps.time.isValid() && gps.date.isValid() && gps.date.year() > 2000) {
    Serial.printf("           UTC %04d-%02d-%02d %02d:%02d:%02d\n",
                  gps.date.year(), gps.date.month(), gps.date.day(),
                  gps.time.hour(), gps.time.minute(), gps.time.second());
  }

  // Health summary every 10s
  if (elapsed % 10 == 0 && elapsed > 0) {
    Serial.printf("           [chars %lu | sentences ok %lu | fail %lu]\n",
                  gps.charsProcessed(),
                  gps.passedChecksum(),
                  gps.failedChecksum());
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  printHeader();
  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  startTime = millis();
  lastStatus = millis();
}

void loop() {
  while (Serial1.available()) {
    gps.encode(Serial1.read());
  }

  if (millis() - lastStatus >= 1000) {
    lastStatus = millis();
    printStatus();
  }

  // Sanity check: if 5s in and we've seen zero bytes, warn loudly.
  unsigned long elapsed = millis() - startTime;
  static bool warned = false;
  if (!warned && elapsed > 5000 && gps.charsProcessed() == 0) {
    warned = true;
    Serial.println();
    Serial.println(" !! No bytes from GPS after 5s. Check wiring/baud/pins.");
    Serial.println();
  }
}
