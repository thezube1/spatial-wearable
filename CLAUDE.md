# Spatial Wearable Project

## Component Reference

This project uses the following hardware components. Refer to the sections below for pinouts, wiring, and configuration details.

---

## 1. Seeed Studio XIAO ESP32S3

**Microcontroller** -- The main brain of the wearable.

### Core Specs

| Parameter | Value |
|-----------|-------|
| Processor | ESP32-S3R8, Xtensa LX7 dual-core, 32-bit, up to 240 MHz |
| Flash | 8 MB (QIO) |
| PSRAM | 8 MB |
| Logic Level | 3.3V |
| Power Input | 5V USB-C or 3.7V LiPo battery |
| 3.3V Regulator Output | Up to 700 mA |
| Dimensions | 21 x 17.8 mm |
| WiFi | 802.11 b/g/n, 2.4 GHz, Station/AP/Station+AP |
| Bluetooth | BLE 5.0 with Mesh |

### Complete Pinout

| Board Pin | GPIO # | Analog | Alternate Functions |
|-----------|--------|--------|---------------------|
| D0 | GPIO1 | A0 | ADC, Touch1 |
| D1 | GPIO2 | A1 | ADC, Touch2 |
| D2 | GPIO3 | A2 | ADC, Touch3 |
| D3 | GPIO4 | A3 | ADC, Touch4 |
| D4 | GPIO5 | A4 | ADC, Touch5, **default I2C SDA** |
| D5 | GPIO6 | A5 | ADC, Touch6, **default I2C SCL** |
| D6 | GPIO43 | -- | **UART TX** |
| D7 | GPIO44 | -- | **UART RX** |
| D8 | GPIO7 | A8 | ADC, Touch7, **SPI SCK** |
| D9 | GPIO8 | A9 | ADC, Touch8, **SPI MISO** |
| D10 | GPIO9 | A10 | ADC, Touch9, **SPI MOSI** |

Sense variant additional pins (expansion board):

| Board Pin | GPIO # | Notes |
|-----------|--------|-------|
| D11 | GPIO42 | PDM Mic CLK; NO ADC support |
| D12 | GPIO41 | PDM Mic DATA; NO ADC support |

### Key Pin Groups

**I2C (default/hardware):**
- SDA = D4 / GPIO5
- SCL = D5 / GPIO6
- Software I2C possible on any GPIO

**SPI (default/hardware):**
- SCK = D8 / GPIO7
- MISO = D9 / GPIO8
- MOSI = D10 / GPIO9
- CS = D2 / GPIO3 (default for SD card)

**UART (Serial1):**
- TX = D6 / GPIO43
- RX = D7 / GPIO44
- Reassignable via `Serial1.begin(baud, config, rxPin, txPin)`

**Power:**
- 5V (VBUS) -- USB 5V input/output
- 3V3 -- 3.3V regulated output (700 mA max)
- GND
- BAT pads (bottom) -- 3.7V LiPo

**ADC:** D0-D5, D8-D10 (9 channels). D6/D7/D11/D12 have NO ADC.

**PWM:** All D0-D10 support PWM (8 LED PWM channels assignable to any GPIO).

**Touch:** D0-D5, D8-D10 (9 capacitive touch pins).

### Important Notes

- GPIO41/GPIO42 do NOT support ADC despite "A" labels
- GPIO3 is a strapping pin (JTAG selection at reset) -- use with caution
- All pins are 3.3V logic -- do NOT apply 5V to GPIO pins
- User LED on GPIO21, active LOW
- Safe pins (no boot conflicts): GPIO1, GPIO2, GPIO4, GPIO5, GPIO6, GPIO7, GPIO8

---

## 2. PCA9548A 8-Channel I2C Multiplexer

**I2C Multiplexer** -- Enables connecting multiple I2C devices that share the same address (e.g., multiple identical displays or sensors).

### Core Specs

| Parameter | Value |
|-----------|-------|
| Channels | 8 independent I2C bus segments |
| VCC Range | 2.3V to 5.5V |
| I/O Tolerance | 5V on all pins |
| I2C Modes | Standard (100 kHz), Fast (400 kHz) |
| Quiescent Current | A few microamps |
| Package | TSSOP24 / SO24 / HVQFN24 |

### I2C Address Configuration

Base address is **0x70**. A0, A1, A2 pins select the address:

| A2 | A1 | A0 | Address |
|----|----|----|---------|
| LOW | LOW | LOW | **0x70** |
| LOW | LOW | HIGH | 0x71 |
| LOW | HIGH | LOW | 0x72 |
| LOW | HIGH | HIGH | 0x73 |
| HIGH | LOW | LOW | 0x74 |
| HIGH | LOW | HIGH | 0x75 |
| HIGH | HIGH | LOW | 0x76 |
| HIGH | HIGH | HIGH | 0x77 |

Formula: `Address = 0x70 + (A2 * 4) + (A1 * 2) + A0`

Up to 8 PCA9548A devices on one bus = 64 downstream channels max.

### Channel Selection

Single 8-bit control register. Each bit = one channel (bit 0 = CH0, bit 7 = CH7).

| Channel | Control Byte |
|---------|-------------|
| CH0 | 0x01 |
| CH1 | 0x02 |
| CH2 | 0x04 |
| CH3 | 0x08 |
| CH4 | 0x10 |
| CH5 | 0x20 |
| CH6 | 0x40 |
| CH7 | 0x80 |
| None | 0x00 |

Multiple channels CAN be enabled simultaneously (rarely desirable). Default after reset: 0x00 (all off).

```cpp
#define PCA9548A_ADDR 0x70

void selectChannel(uint8_t channel) {
  if (channel > 7) return;
  Wire.beginTransmission(PCA9548A_ADDR);
  Wire.write(1 << channel);
  Wire.endTransmission();
}
```

### Wiring

```
XIAO ESP32S3                PCA9548A
───────────                 ────────
D4 (GPIO5/SDA) ──[4.7k]── SDA (pin 4)
D5 (GPIO6/SCL) ──[4.7k]── SCL (pin 6)
3V3            ─────────── VCC (pin 2)
GND            ─────────── VSS (pin 23)
Any GPIO       ─────────── RESET (pin 5) [also pull-up to VCC]
GND/VCC        ─────────── A0, A1, A2 (per desired address)

Each downstream channel (SD0/SC0 .. SD7/SC7):
  SDn ──[4.7k pull-up to VCC]── Slave SDA
  SCn ──[4.7k pull-up to VCC]── Slave SCL
```

### Important Notes

- **NOT a buffer/repeater** -- uses pass-FET switches. Active channel capacitance adds to upstream bus.
- **Pull-ups needed on EVERY segment** -- upstream AND each downstream channel independently.
- **RESET pin** is active-LOW. Pull high for normal operation. Connect to a GPIO for bus recovery.
- **Address 0x70 conflicts** with PCA9685 servo drivers' all-call address. Use 0x71+ if needed.
- **Only switch channels when bus is idle** (after STOP condition).
- **Bus lock-up recovery**: If a slave shorts SDA/SCL low on an active channel, the mux becomes unreachable. Only hardware RESET or power cycle recovers. Always wire the RESET pin to a GPIO.
- **Cascading**: Possible but cumulative FET resistance causes voltage drops. Keep to 1-2 levels max.
- **PCA9548A vs TCA9548A**: Functionally identical. TCA variant supports down to 1.65V.

---

## 3. GC9A01 Round TFT LCD Display (Teyleten Robot)

**Display** -- 1.28" round IPS TFT driven by the GC9A01A IC.

### Display Specs

| Parameter | Value |
|-----------|-------|
| Size | 1.28 inches diagonal |
| Shape | Round (circular) |
| Resolution | 240 x 240 pixels |
| Panel Type | IPS (178-degree viewing angle) |
| Color Depth | 16-bit RGB565 (65K colors), also supports 18-bit RGB666 |
| Driver IC | GC9A01A (GalaxyCore) |
| Interface | 4-wire SPI (write-only) |
| Module Dimensions | 38 x 45.5 x 3.2 mm |

### Pinout (7-pin Teyleten Robot module)

| Pin | Name | Function |
|-----|------|----------|
| 1 | VCC | Power (3.3V or 5V -- has onboard regulator) |
| 2 | GND | Ground |
| 3 | SCL (SCK) | SPI clock |
| 4 | SDA (MOSI) | SPI data in (Master Out Slave In) |
| 5 | DC | Data/Command select (HIGH=data, LOW=command) |
| 6 | CS | Chip Select (active LOW) |
| 7 | RST | Hardware reset (active LOW) |

**Note:** The 7-pin Teyleten module has NO separate backlight pin -- backlight is always on when VCC is powered. 8-pin variants from other manufacturers expose a BL/BLK pin for PWM dimming.

### SPI Details

- SPI Mode 0 (CPOL=0, CPHA=0), MSB first
- Rated max clock: ~20 MHz (datasheet)
- Practical: 27-40 MHz on ESP32 works reliably
- Write-only (no MISO needed)
- Full-screen 240x240 RGB565 framebuffer = 115,200 bytes

### Wiring to XIAO ESP32S3

```
GC9A01 Module         XIAO ESP32S3
─────────────         ────────────
VCC          ───────  3V3
GND          ───────  GND
SCL (SCK)    ───────  D8 (GPIO7 / SPI SCK)
SDA (MOSI)   ───────  D10 (GPIO9 / SPI MOSI)
CS           ───────  D1 (GPIO2) or any free GPIO
DC           ───────  D2 (GPIO3) or any free GPIO
RST          ───────  D3 (GPIO4) or any free GPIO
```

### Library Configuration (TFT_eSPI)

```cpp
// In User_Setup.h
#define GC9A01_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 240

// XIAO ESP32S3 pin assignments
#define TFT_MOSI   9   // GPIO9 = D10
#define TFT_SCLK   7   // GPIO7 = D8
#define TFT_CS     2   // GPIO2 = D1
#define TFT_DC     3   // GPIO3 = D2
#define TFT_RST    4   // GPIO4 = D3

#define SPI_FREQUENCY       40000000  // 40 MHz
#define SPI_READ_FREQUENCY  20000000
```

### Adafruit Library Setup

```cpp
#include <Adafruit_GC9A01A.h>
#include <SPI.h>

#define TFT_CS   2
#define TFT_DC   3
#define TFT_RST  4

Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);

void setup() {
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(GC9A01A_BLACK);
}
```

### Recommended Libraries

1. **TFT_eSPI** (Bodmer) -- Fastest on ESP32, DMA support, sprites for flicker-free rendering
2. **Adafruit_GC9A01A** -- Simpler setup, Adafruit_GFX API
3. **Arduino_GFX** (moononournation) -- Multi-platform
4. **LVGL** -- Full widget toolkit, ideal for round watch-face UIs (pairs with TFT_eSPI)

### Important Notes

- Framebuffer is a 240x240 square; corner pixels are hidden behind the circular mask. Clipping radius ~120px from center (120,120).
- `setRotation(0)` through `setRotation(3)` rotates 90-degree increments.
- Native pixel format: **RGB565** (16-bit).
- GC9A01A requires an extensive init sequence (~50+ register writes) -- all major libraries handle this.
- Sleep mode: command 0x10 (enter), 0x11 (exit) -- drops IC to micro-amp consumption. Backlight stays on (7-pin module).
- Signal pins are 3.3V logic. ESP32S3 is 3.3V native, so no level shifter needed.

---

## 4. GOOUUU GPS+BD Module

**GNSS Module** -- Dual-mode GPS + BeiDou positioning module based on the AT6558 chipset.

### Identification

The "GOOUUU GPS+BD" is the dual-constellation variant from Goouuu Tech, based on the **AT6558** GNSS SOC (often packaged as **ATGM336H**). Do NOT confuse with the GT-U7 which is GPS-only (u-blox based).

### Core Specs

| Parameter | Value |
|-----------|-------|
| Chipset | AT6558 (ZhongKe Microelectronics) |
| GNSS Systems | GPS, BeiDou, GLONASS, Galileo, QZSS, SBAS |
| Interface | UART (TTL) |
| Default Baud Rate | 9600 bps (configurable 4800-115200) |
| Data Format | 8N1 |
| Protocol | NMEA 0183 |
| Update Rate | 1 Hz default, up to 10 Hz |
| Position Accuracy | 2.5m CEP50 (open sky) |
| Speed Accuracy | <0.1 m/s |
| Timing Precision | <30 ns |
| Max Altitude | 18,000 m |
| Max Velocity | 515 m/s |
| Cold Start | <=35 seconds |
| Warm Start | ~1 second |
| Hot Start | <=1 second |
| Cold Start Sensitivity | -148 dBm |
| Tracking Sensitivity | -162 dBm |
| Supply Voltage | 3.3V-5V (breakout board) / 2.7V-3.6V (bare module) |
| Current (tracking) | <25 mA at 3.3V |
| Current (peak) | 100 mA |

### Pinout (Breakout Board -- 5-pin header)

| Pin | Name | Function |
|-----|------|----------|
| 1 | VCC | Power input (3.3V-5V) |
| 2 | GND | Ground |
| 3 | TXD | UART data output (connect to MCU RX) |
| 4 | RXD | UART data input (connect to MCU TX) |
| 5 | PPS | Pulse-Per-Second timing output |

Additional pins on bare module (AT6558/ATGM336H):
- ON/OFF -- Module shutdown control (active LOW)
- VBAT -- RTC/SRAM backup battery supply
- nRESET -- Hardware reset (active LOW)
- RF_IN -- Antenna RF input
- VCC_RF -- 3.3V output for active antenna
- SDA -- I2C data (some variants)

### Antenna

- Onboard ceramic patch antenna (built-in)
- IPEX (U.FL) connector for external active antenna (usually included)
- Built-in antenna detection and short-circuit protection

### NMEA Output

Three talker-ID prefixes: **GP** (GPS), **BD** (BeiDou), **GN** (combined dual-mode).

Sentence types: GGA, RMC, GSA, GSV, GLL, VTG.

### Wiring to XIAO ESP32S3

```
GPS Module            XIAO ESP32S3
──────────            ────────────
VCC          ───────  3V3
GND          ───────  GND
TXD          ───────  D7 (GPIO44 / UART RX)
RXD          ───────  D6 (GPIO43 / UART TX)
PPS          ───────  Any free GPIO (optional)
```

```cpp
// Using hardware Serial1 on XIAO ESP32S3
#define GPS_RX_PIN 44  // D7 -- connects to GPS TXD
#define GPS_TX_PIN 43  // D6 -- connects to GPS RXD

void setup() {
  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
}
```

### Recommended Libraries

1. **TinyGPS++** (Mikal Hart) -- Most popular. Parses NMEA for lat, lon, altitude, speed, course, date, time, satellites, HDOP. Use a recent version that handles GN talker IDs.
2. **NeoGPS** -- More memory-efficient, supports more sentence types.
3. **MicroNMEA** -- Lightweight parser.

### Important Notes

- Default baud rate is 9600. Increase to 38400+ if using update rates >1 Hz to avoid data overflow.
- Backup battery maintains RTC/ephemeris for fast warm/hot starts. Without it, every start is cold (~35s).
- Needs clear sky view for fix. Indoors requires external active antenna.
- TinyGPS++ parses GP-prefixed sentences by default; ensure your version handles GN-prefixed (dual-mode) sentences.
- Supports A-GNSS for faster initial fixes when ephemeris data is injected.

---

## Wiring Summary (All Components to XIAO ESP32S3)

```
XIAO ESP32S3 Pin Assignments:
─────────────────────────────

I2C Bus (to PCA9548A upstream):
  D4 (GPIO5)  = SDA  ──[4.7k pull-up]── PCA9548A SDA
  D5 (GPIO6)  = SCL  ──[4.7k pull-up]── PCA9548A SCL

SPI Bus (to GC9A01 displays via mux or direct):
  D8  (GPIO7) = SCK   ── Display SCL
  D10 (GPIO9) = MOSI  ── Display SDA
  D1  (GPIO2) = CS    ── Display CS
  D2  (GPIO3) = DC    ── Display DC
  D3  (GPIO4) = RST   ── Display RST

UART (to GPS module):
  D6 (GPIO43) = TX ── GPS RXD
  D7 (GPIO44) = RX ── GPS TXD

Free for additional use:
  D0 (GPIO1)  = Available (ADC, Touch, digital)
  D9 (GPIO8)  = Available (SPI MISO if needed)

Power:
  3V3 ── PCA9548A VCC, GPS VCC, Display VCC
  GND ── All component grounds
```

**Note:** If driving multiple GC9A01 displays via the PCA9548A I2C mux, that won't work directly since the displays use SPI, not I2C. For multiple SPI displays, either use separate CS lines for each display, or use an SPI multiplexer. The PCA9548A is for multiplexing I2C devices (e.g., multiple I2C sensors sharing the same address).
