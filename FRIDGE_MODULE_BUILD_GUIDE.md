# 🧊 Fridge Module Build Guide
### Smart Medicine Storage System — Fridge Node (ESP32 + DS18B20 + DHT22 + LoRa)

> **Who this is for:** The person building and flashing the **Fridge ESP32 node**.
> The Hub node is handled separately. This guide covers everything you need: libraries, wiring, code, and upload steps.

---

## 📦 What You Need (Your Parts)

| Component | Qty | Notes |
|---|---|---|
| ESP32 DevKit V1 board | 1 | Same board from the original project |
| DS18B20 temperature sensor | 1 | Already owned — the silver probe |
| 4.7 kΩ resistor | 1 | Already owned — pull-up for DS18B20 |
| DHT22 / AM2302 humidity sensor | 1 | **New purchase** — ~₹100–₹150 |
| 10 kΩ resistor | 1 | Only needed if your DHT22 is a bare component, not a module with a PCB board |
| SX1278 / RA-02 LoRa module (433 MHz) | 1 | One of the two RA-02 boards |
| 433 MHz antenna | 1 | The short black antenna that screws/connects to the RA-02 |
| RF pigtail cable (if needed) | 1 | Connects antenna to the RA-02 RF pad |
| Breadboard | 1 | Already owned |
| Jumper wires (male–male, male–female) | Several | Already owned |
| USB cable + PC for flashing | 1 | Already owned |

> **WARNING: NEVER transmit with the RA-02 without an antenna physically attached. You can permanently damage the radio module.**

---

## 💻 Software Setup (Do This First)

### Step 1 — Install Arduino IDE

Download **Arduino IDE 2.x** from https://www.arduino.cc/en/software
Install it and open it.

---

### Step 2 — Add ESP32 Board Package

1. Open Arduino IDE → **File → Preferences**
2. In the field **"Additional boards manager URLs"**, paste:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Click **OK**
4. Go to **Tools → Board → Boards Manager**
5. Search for **esp32** → Install **"esp32 by Espressif Systems"**
   - Install version **2.x** (e.g. 2.0.17). Avoid version 3.x for now.
6. After install: **Tools → Board → ESP32 Arduino → ESP32 Dev Module**

---

### Step 3 — Install Required Libraries

Go to **Tools → Manage Libraries**, search for and install each of these:

| Library Name to Search | Install Author |
|---|---|
| `OneWire` | Paul Stoffregen |
| `DallasTemperature` | Miles Burton |
| `DHT sensor library` | Adafruit |
| `Adafruit Unified Sensor` | Adafruit |
| `LoRa` | Sandeep Mistry |

> **Important:** When installing `DHT sensor library`, Arduino IDE will ask "Install all?" — click **Install All** to also install the `Adafruit Unified Sensor` dependency automatically.

#### Verify Installation

After installing, go to **Sketch → Include Library → Manage Libraries** and confirm you see:
- OneWire ✓
- DallasTemperature ✓
- DHT sensor library (Adafruit) ✓
- Adafruit Unified Sensor ✓
- LoRa (Sandeep Mistry) ✓

---

## 🔌 Wiring Connections

**CRITICAL RULE: The RA-02 LoRa module runs on 3.3V only. Do NOT connect it to 5V.**

---

### A. DS18B20 Temperature Sensor Wiring

The DS18B20 is the silver bullet-shaped probe. It has 3 wires.

| DS18B20 | Wire Color | Connect To |
|---|---|---|
| VCC | Red | ESP32 **3.3V** |
| GND | Black | ESP32 **GND** |
| DATA | Yellow or White | ESP32 **GPIO 4** |

**Pull-up resistor — mandatory:**
Place a **4.7 kΩ resistor** between DS18B20 DATA (GPIO 4) and the 3.3V rail.

```
3.3V ──┬──[4.7kΩ]──┬── GPIO 4
       │            │
    DS18B20 VCC   DS18B20 DATA
```

---

### B. DHT22 Humidity Sensor Wiring

**Check your DHT22 first:**
- If it's a **module/breakout board** (has a PCB with 3 labelled pins), the pull-up resistor is built in — skip the 10kΩ resistor.
- If it's a **bare 4-pin component** (no PCB), add the 10kΩ pull-up.

**Bare 4-pin DHT22 (has 4 legs):**

| DHT22 Pin | Name | Connect To |
|---|---|---|
| Pin 1 (leftmost) | VCC | ESP32 **3.3V** |
| Pin 2 | DATA | ESP32 **GPIO 16** |
| Pin 3 | NC | Not connected |
| Pin 4 (rightmost) | GND | ESP32 **GND** |

Add a **10kΩ resistor** between GPIO 16 and 3.3V.

**DHT22 Module (3-pin breakout board):**

| Module Label | Connect To |
|---|---|
| VCC or + | ESP32 **3.3V** |
| DATA or S or OUT | ESP32 **GPIO 16** |
| GND or - | ESP32 **GND** |

No extra resistor needed.

---

### C. RA-02 / SX1278 LoRa Module (SPI Wiring)

| RA-02 Label | ESP32 GPIO |
|---|---|
| VCC | **3.3V** |
| GND | **GND** |
| SCK | **GPIO 18** |
| MISO | **GPIO 19** |
| MOSI | **GPIO 23** |
| NSS (or CS) | **GPIO 15** *(updated from GPIO 5)* |
| RST (or RESET) | **GPIO 14** |
| DIO0 | **GPIO 26** |

Attach the **433 MHz antenna** to the RF connector before powering on.

---

### Full Wiring Summary

| Connection | From | To |
|---|---|---|
| DS18B20 power | DS18B20 VCC | ESP32 3.3V |
| DS18B20 ground | DS18B20 GND | ESP32 GND |
| DS18B20 data | DS18B20 DATA | GPIO 4 |
| DS18B20 pull-up | 4.7kΩ resistor | between GPIO4 and 3.3V |
| DHT22 power | DHT22 VCC | ESP32 3.3V |
| DHT22 ground | DHT22 GND | ESP32 GND |
| DHT22 data | DHT22 DATA | GPIO 16 |
| DHT22 pull-up | 10kΩ resistor (bare only) | between GPIO16 and 3.3V |
| LoRa power | RA-02 VCC | ESP32 3.3V |
| LoRa ground | RA-02 GND | ESP32 GND |
| LoRa clock | RA-02 SCK | GPIO 18 |
| LoRa data out | RA-02 MISO | GPIO 19 |
| LoRa data in | RA-02 MOSI | GPIO 23 |
| LoRa chip select | RA-02 NSS | GPIO 15 (updated from 5) |
| LoRa reset | RA-02 RST | GPIO 14 |
| LoRa interrupt | RA-02 DIO0 | GPIO 26 |

---

## Pre-Power Checklist

Before connecting USB power:

- [ ] Antenna connected to RA-02
- [ ] RA-02 VCC → 3.3V (NOT 5V)
- [ ] All GNDs share common ground on breadboard
- [ ] DS18B20 DATA has 4.7kΩ pull-up to 3.3V
- [ ] DHT22 pull-up added if using bare sensor
- [ ] No wires accidentally bridging VCC and GND

---

## 📝 The Firmware Code

Create a new Arduino sketch: **File → New Sketch**
Name it `Fridge_Sender`
Delete all existing content and paste the code below:

```cpp
// ================================================================
//  FRIDGE NODE FIRMWARE — Smart Medicine Storage System
//  Sensors  : DS18B20 (temperature) + DHT22 (humidity)
//  Radio    : SX1278 / RA-02 LoRa at 433 MHz via SPI
//  Sends    : Pipe-delimited packet every 5 seconds
//  Device ID: FRIDGE_001
// ================================================================

#include <SPI.h>
#include <LoRa.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>

// ── PIN DEFINITIONS ──
#define ONE_WIRE_BUS  4     // DS18B20 data pin
#define DHT_PIN       16    // DHT22 data pin
#define DHT_TYPE      DHT22

// LoRa SPI pins for ESP32
#define LORA_SS       5
#define LORA_RST      14
#define LORA_DIO0     26

// ── DEVICE IDENTITY ──
const char* DEVICE_ID = "FRIDGE_001";

// ── LORA RADIO SETTINGS (must MATCH the Hub exactly) ──
#define LORA_FREQUENCY  433E6   // 433 MHz
#define LORA_SF         7       // Spreading factor 7
#define LORA_BW         125E3   // Bandwidth 125 kHz
#define LORA_CR         5       // Coding rate 4/5
#define LORA_TX_POWER   17      // TX power dBm

// ── SENSOR OBJECTS ──
OneWire           oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);
DHT               dht(DHT_PIN, DHT_TYPE);

// ── TIMING ──
unsigned long lastSend = 0;
const unsigned long SEND_INTERVAL = 5000; // 5 seconds

// ── PACKET COUNTER ──
uint32_t sequence = 0;

// ================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== FRIDGE NODE STARTING ===");
  Serial.print("Device ID: ");
  Serial.println(DEVICE_ID);

  // DS18B20 Init
  ds18b20.begin();
  Serial.println("[DS18B20] Initialized");

  // DHT22 Init
  dht.begin();
  Serial.println("[DHT22] Initialized");

  // LoRa Init
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  Serial.print("[LoRa] Initializing at 433 MHz... ");
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("FAILED!");
    Serial.println("Check: RA-02 wiring, 3.3V supply, antenna connected.");
    while (true) {
      delay(1000);
      Serial.println("[LoRa] Still not ready. Halted.");
    }
  }

  // Configure radio (must match hub)
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.enableCrc(); // Enable CRC error detection

  Serial.println("OK");
  Serial.println("=== FRIDGE NODE READY ===\n");
}

// ================================================================
void loop() {
  unsigned long now = millis();
  if (now - lastSend >= SEND_INTERVAL) {
    lastSend = now;
    sendSensorPacket();
  }
}

// ================================================================
void sendSensorPacket() {
  // Read DS18B20
  ds18b20.requestTemperatures();
  float tempC = ds18b20.getTempCByIndex(0);
  bool tempValid = (tempC != DEVICE_DISCONNECTED_C && tempC > -55.0 && tempC < 125.0);

  // Read DHT22
  float humidity = dht.readHumidity();
  float dhtTemp  = dht.readTemperature();
  bool humValid  = (!isnan(humidity) && humidity >= 0.0 && humidity <= 100.0);

  // Increment sequence
  sequence++;

  // Build status string
  String sensorStatus = "OK";
  if (!tempValid && !humValid) sensorStatus = "DS18B20_ERR,DHT22_ERR";
  else if (!tempValid)         sensorStatus = "DS18B20_ERR";
  else if (!humValid)          sensorStatus = "DHT22_ERR";

  // Serial debug output
  Serial.println("--- Sending Packet ---");
  Serial.print("Seq     : "); Serial.println(sequence);
  Serial.print("DS18B20 : "); Serial.print(tempValid ? tempC : -999.0); Serial.println(" C");
  Serial.print("DHT22 H : "); Serial.print(humValid  ? humidity : -999.0); Serial.println(" %");
  Serial.print("Status  : "); Serial.println(sensorStatus);

  // Build packet: DEVICE_ID|SEQ|TEMP_C|HUMIDITY|STATUS
  String packet = String(DEVICE_ID)
                + "|" + String(sequence)
                + "|" + String(tempValid ? tempC    : -999.0, 2)
                + "|" + String(humValid  ? humidity : -999.0, 2)
                + "|" + sensorStatus;

  // Transmit via LoRa
  LoRa.beginPacket();
  LoRa.print(packet);
  bool sent = LoRa.endPacket();

  if (sent) {
    Serial.print("[LoRa] Sent OK: ");
    Serial.println(packet);
  } else {
    Serial.println("[LoRa] Send FAILED");
  }
  Serial.println();
}
```

---

## Uploading to the ESP32

### Arduino IDE Board Settings

Go to **Tools** menu and set:

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module |
| Upload Speed | 115200 |
| CPU Frequency | 240 MHz (WiFi/BT) |
| Flash Mode | QIO |
| Flash Size | 4MB (32Mb) |
| Partition Scheme | Default 4MB with spiffs |
| Port | Your ESP32 USB COM port |

### Finding Your COM Port

- **Windows:** Device Manager → Ports (COM & LPT) → CP210x or CH340 serial
- **Mac:** Terminal → `ls /dev/cu.*` → look for `/dev/cu.usbserial-XXXXXX`
- **Linux:** `/dev/ttyUSB0` or `/dev/ttyACM0`

### Upload Steps

1. Click **Upload** (arrow button) in Arduino IDE
2. When you see `Connecting.....` in the log, hold the **BOOT** button on the ESP32
3. Release BOOT when uploading starts
4. Wait for `Done uploading`
5. Open **Tools → Serial Monitor**, set baud to **115200**
6. Press the **EN (Reset)** button on the ESP32

### Expected Serial Monitor Output

```
=== FRIDGE NODE STARTING ===
Device ID: FRIDGE_001
[DS18B20] Initialized
[DHT22] Initialized
[LoRa] Initializing at 433 MHz... OK
=== FRIDGE NODE READY ===

--- Sending Packet ---
Seq     : 1
DS18B20 : 23.50 C
DHT22 H : 52.30 %
Status  : OK
[LoRa] Sent OK: FRIDGE_001|1|23.50|52.30|OK
```

---

## Troubleshooting

| What You See | Likely Cause | Fix |
|---|---|---|
| DS18B20 reads -127 or -999 | Sensor not detected | Check GPIO4 wiring; check 4.7kΩ pull-up; check VCC and GND |
| DHT22 humidity shows -999 | Sensor not connected | Check GPIO16 wiring; add 10kΩ if bare sensor |
| `[LoRa] FAILED!` at startup | RA-02 wiring problem | Check SPI pins; confirm VCC=3.3V not 5V; check NSS=GPIO5 |
| `[LoRa] Send FAILED` | Intermittent TX issue | Wait a few cycles; check antenna connection |
| Nothing in Serial Monitor | Wrong baud rate or COM port | Set to 115200; select correct port; press EN button |
| Upload fails (no connection) | Need to hold BOOT | Hold BOOT button when you see `Connecting.....` |

---

## LoRa Settings Reference (Share With Hub Builder)

Both the fridge node and hub must use **identical radio settings**:

| Parameter | Value |
|---|---|
| Frequency | 433 MHz |
| Spreading Factor | 7 |
| Bandwidth | 125 kHz |
| Coding Rate | 4/5 |
| TX Power | 17 dBm |
| CRC | Enabled |

---

## Packet Format (for Hub Builder Reference)

The fridge sends pipe-delimited text over LoRa:

```
DEVICE_ID|SEQUENCE|TEMP_C|HUMIDITY_RH|STATUS
```

**Examples:**

Normal operation:
```
FRIDGE_001|125|4.20|48.50|OK
```

DS18B20 disconnected:
```
FRIDGE_001|126|-999.00|48.50|DS18B20_ERR
```

Both sensors failed:
```
FRIDGE_001|127|-999.00|-999.00|DS18B20_ERR,DHT22_ERR
```

**Field descriptions:**

| Field | Type | Description |
|---|---|---|
| DEVICE_ID | String | `FRIDGE_001` (or FRIDGE_002 for future nodes) |
| SEQUENCE | Integer | Increments every packet |
| TEMP_C | Float (2dp) | DS18B20 temperature in °C. `-999.00` = error |
| HUMIDITY_RH | Float (2dp) | DHT22 relative humidity %. `-999.00` = error |
| STATUS | String | OK, DS18B20_ERR, DHT22_ERR, or DS18B20_ERR,DHT22_ERR |

---

## Handoff Checklist — Fridge Module Done

Confirm all of these before handing off:

- [ ] DS18B20 reads realistic temperature (e.g. 20–30°C in room)
- [ ] DHT22 reads realistic humidity (e.g. 40–70% indoors)
- [ ] LoRa initializes with `OK` — no FAILED message
- [ ] Serial monitor shows `[LoRa] Sent OK: FRIDGE_001|...` every 5 seconds
- [ ] Status shows `OK` (not error codes)
- [ ] Packet format and LoRa settings confirmed and shared with Hub builder
- [ ] Antenna is physically attached and correct frequency (433 MHz)

---

## Coordinate With the Hub Builder

Tell the Hub builder:
1. Your **LoRa radio settings** (all listed in the table above)
2. The **packet format** (pipe-delimited string shown above)
3. Your **Device ID** = `FRIDGE_001`
4. Do a **short-range test** first — same room, 1–2 metres — before testing distance

---

*Smart Medicine Storage System — Fridge Module Build Guide — October 2026*
