# 🖥️ Hub Module — Connections & Testing Guide
### Smart Medicine Storage System — Hub Node (ESP32 + LoRa + LCD + Buzzer)

> **Project is tomorrow. Follow this in order. Do not skip steps.**

---

## 📦 Hub Parts Checklist

Before you start, confirm you have everything on the table:

| Component | Status |
|---|---|
| ESP32 DevKit V1 (Hub board) | Already owned |
| 16×2 I2C LCD (address 0x27) | Already owned |
| Active buzzer module | Already owned |
| SX1278 / RA-02 LoRa module (433 MHz) | New — second of the two RA-02 boards |
| 433 MHz antenna | New — second antenna |
| RF pigtail cable (if needed) | New |
| Breadboard + jumper wires | Already owned |
| USB cable + PC | Already owned |

---

## 🔌 Hub Wiring — All Connections

> **RULE: RA-02 VCC → 3.3V only. Never 5V.**

---

### A. 16×2 I2C LCD Wiring (unchanged from original)

The LCD uses I2C — only 4 wires.

| LCD Module Pin | Connect To |
|---|---|
| VCC | ESP32 **5V (Vin)** or **3.3V** (check your module) |
| GND | ESP32 **GND** |
| SDA | ESP32 **GPIO 21** |
| SCL | ESP32 **GPIO 22** |

> Most 16×2 I2C LCD modules work on 5V backlight but 3.3V I2C logic — connect VCC to the Vin (5V from USB) pin and it works fine with the ESP32.

---

### B. Active Buzzer Wiring (unchanged from original)

| Buzzer Pin | Connect To |
|---|---|
| VCC or + | ESP32 **3.3V** (or 5V if it's a 5V buzzer) |
| GND or - | ESP32 **GND** |
| Signal (I/O) | ESP32 **GPIO 27** |

> If your buzzer module only has 2 pins (no signal pin), connect the + to GPIO 27 and − to GND directly — it beeps when GPIO 27 goes HIGH.

---

### C. RA-02 / SX1278 LoRa Module (SPI — same pins as fridge node)

| RA-02 Label | ESP32 GPIO |
|---|---|
| VCC | **3.3V** |
| GND | **GND** |
| SCK | **GPIO 18** |
| MISO | **GPIO 19** |
| MOSI | **GPIO 23** |
| NSS (CS) | **GPIO 15** *(updated from GPIO 5)* |
| RST (RESET) | **GPIO 14** |
| DIO0 | **GPIO 26** |

Attach the **433 MHz antenna** before powering on.

---

### Full Hub Wiring Summary Table

| What | From | To |
|---|---|---|
| LCD power | LCD VCC | ESP32 Vin (5V) |
| LCD ground | LCD GND | ESP32 GND |
| LCD data | LCD SDA | GPIO 21 |
| LCD clock | LCD SCL | GPIO 22 |
| Buzzer ground | Buzzer GND | ESP32 GND |
| Buzzer signal | Buzzer I/O | GPIO 27 |
| Buzzer power | Buzzer VCC | ESP32 3.3V |
| LoRa power | RA-02 VCC | ESP32 3.3V |
| LoRa ground | RA-02 GND | ESP32 GND |
| LoRa clock | RA-02 SCK | GPIO 18 |
| LoRa data out | RA-02 MISO | GPIO 19 |
| LoRa data in | RA-02 MOSI | GPIO 23 |
| LoRa chip select | RA-02 NSS | GPIO 15 (updated from 5) |
| LoRa reset | RA-02 RST | GPIO 14 |
| LoRa interrupt | RA-02 DIO0 | GPIO 26 |

### Pin Conflict Check — All Clear ✅

| GPIO | Used For |
|---|---|
| GPIO 4 | ❌ NOT used on hub (fridge only) |
| GPIO 5 | Free (moved LoRa NSS to GPIO 15) |
| GPIO 14 | LoRa RST |
| GPIO 15 | LoRa NSS |
| GPIO 18 | LoRa SCK |
| GPIO 19 | LoRa MISO |
| GPIO 21 | LCD SDA |
| GPIO 22 | LCD SCL |
| GPIO 23 | LoRa MOSI |
| GPIO 26 | LoRa DIO0 |
| GPIO 27 | Buzzer |

No overlaps.

---

## 💻 Software Setup

### Step 1 — Arduino IDE + ESP32 Board

Same as fridge node setup. If already done, skip to Step 2.

- Arduino IDE 2.x → Preferences → add ESP32 board URL
- Install **esp32 by Espressif Systems v2.x**
- Board: **ESP32 Dev Module**

### Step 2 — Install Required Libraries

Go to **Tools → Manage Libraries**, install:

| Search For | Author |
|---|---|
| `LoRa` | Sandeep Mistry |
| `LiquidCrystal_PCF8574` | Matthias Hertel |
| `ESPAsyncWebServer` | Me-No-Dev (install manually — see below) |
| `AsyncTCP` | Me-No-Dev (install manually — see below) |

#### ESPAsyncWebServer & AsyncTCP — Manual Install

These two are not in the Arduino Library Manager. Install manually:

1. Download ZIP from GitHub:
   - **AsyncTCP:** https://github.com/me-no-dev/AsyncTCP/archive/refs/heads/master.zip
   - **ESPAsyncWebServer:** https://github.com/me-no-dev/ESPAsyncWebServer/archive/refs/heads/master.zip
2. Arduino IDE → **Sketch → Include Library → Add .ZIP Library...**
3. Select the downloaded ZIP for each

---

## 📝 Hub Firmware Code

Create a new sketch: **File → New Sketch**
Name it `Hub_Receiver_Server`
Delete everything and paste:

```cpp
// ================================================================
//  HUB NODE FIRMWARE — Smart Medicine Storage System
//  Receives  : LoRa packets from FRIDGE_001
//  Displays  : Temperature + Humidity on I2C LCD
//  Alerts    : Buzzer if temperature or humidity exceeds threshold
//  Serves    : Local web dashboard over WiFi AP
//  Packet    : DEVICE_ID|SEQ|TEMP_C|HUMIDITY|STATUS
// ================================================================

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <LiquidCrystal_PCF8574.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>

// ── LCD ──
LiquidCrystal_PCF8574 lcd(0x27);  // I2C address 0x27

// ── BUZZER ──
#define BUZZER_PIN 27

// ── LORA PINS ──
#define LORA_SS    5
#define LORA_RST   14
#define LORA_DIO0  26

// ── LORA RADIO SETTINGS (must MATCH fridge node exactly) ──
#define LORA_FREQUENCY  433E6
#define LORA_SF         7
#define LORA_BW         125E3
#define LORA_CR         5

// ── WIFI ACCESS POINT ──
const char* AP_SSID     = "Medicine_Box";
const char* AP_PASSWORD = "12345678";

// ── THRESHOLDS (configurable via dashboard) ──
float tempThreshold = 30.0;   // Alert if temp exceeds this
float humThreshold  = 80.0;   // Alert if humidity exceeds this

// ── DEVICE STATE ──
struct DeviceState {
  String  id;
  float   temperature;
  float   humidity;
  String  status;
  uint32_t sequence;
  int     rssi;
  bool    online;
  unsigned long lastSeen;
};

DeviceState fridge;

// ── WEB SERVER ──
AsyncWebServer server(80);

// ── TIMING ──
unsigned long lastLcdUpdate   = 0;
unsigned long lcdDisplayToggle = 0;
bool showHumidity = false;       // Toggles LCD between temp and humidity

// ── ALERT FLAGS ──
bool tempAlert = false;
bool humAlert  = false;

// ================================================================
// HELPERS
// ================================================================

// Parse incoming LoRa packet: DEVICE_ID|SEQ|TEMP_C|HUMIDITY|STATUS
bool parsePacket(String raw) {
  // Split by |
  int f[5];   // field start positions
  int fc = 0;
  f[fc++] = 0;
  for (int i = 0; i < raw.length() && fc < 5; i++) {
    if (raw[i] == '|') f[fc++] = i + 1;
  }
  if (fc < 5) return false;  // Not enough fields

  fridge.id          = raw.substring(f[0], raw.indexOf('|', f[0]));
  fridge.sequence    = raw.substring(f[1], raw.indexOf('|', f[1])).toInt();
  fridge.temperature = raw.substring(f[2], raw.indexOf('|', f[2])).toFloat();
  fridge.humidity    = raw.substring(f[3], raw.indexOf('|', f[3])).toFloat();
  fridge.status      = raw.substring(f[4]);
  fridge.lastSeen    = millis();
  fridge.online      = true;

  return true;
}

// Build JSON status response for the API
String buildStatusJSON() {
  bool offline = (millis() - fridge.lastSeen > 30000); // 30s timeout
  if (offline) fridge.online = false;

  String json = "{";
  json += "\"id\":\"" + fridge.id + "\",";
  json += "\"temperature\":" + String(fridge.temperature, 1) + ",";
  json += "\"humidity\":" + String(fridge.humidity, 1) + ",";
  json += "\"sequence\":" + String(fridge.sequence) + ",";
  json += "\"rssi\":" + String(fridge.rssi) + ",";
  json += "\"status\":\"" + fridge.status + "\",";
  json += "\"online\":" + String(fridge.online ? "true" : "false") + ",";
  json += "\"tempThreshold\":" + String(tempThreshold, 1) + ",";
  json += "\"humThreshold\":" + String(humThreshold, 1) + ",";
  json += "\"tempAlert\":" + String(tempAlert ? "true" : "false") + ",";
  json += "\"humAlert\":" + String(humAlert ? "true" : "false");
  json += "}";
  return json;
}

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== HUB NODE STARTING ===");

  // ── Buzzer Init ──
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // ── LCD Init ──
  Wire.begin(21, 22);
  lcd.begin(16, 2);
  lcd.setBacklight(255);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Hub Starting...");
  Serial.println("[LCD] OK");

  // ── SPIFFS Init ──
  if (!SPIFFS.begin(true)) {
    Serial.println("[SPIFFS] FAILED — dashboard won't load");
  } else {
    Serial.println("[SPIFFS] OK");
    // List files in SPIFFS for debug
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    while (file) {
      Serial.print("[SPIFFS] Found: ");
      Serial.println(file.name());
      file = root.openNextFile();
    }
  }

  // ── WiFi Access Point ──
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("[WiFi] AP started: ");
  Serial.print(AP_SSID);
  Serial.print(" | IP: ");
  Serial.println(WiFi.softAPIP());
  lcd.setCursor(0, 1);
  lcd.print(WiFi.softAPIP());
  delay(2000);

  // ── LoRa Init ──
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  Serial.print("[LoRa] Initializing at 433 MHz... ");
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("FAILED!");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("LoRa FAILED!");
    while (true) delay(1000);
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.enableCrc();
  Serial.println("OK");

  // ── Web Server Routes ──

  // Dashboard (served from SPIFFS)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (SPIFFS.exists("/index.html")) {
      req->send(SPIFFS, "/index.html", "text/html");
    } else {
      req->send(200, "text/html",
        "<h2>Medicine Hub Online</h2>"
        "<p>Dashboard file not found on SPIFFS.</p>"
        "<p>Use <a href='/status'>/status</a> to see JSON data.</p>");
    }
  });

  // Temperature only (legacy — kept for compatibility)
  server.on("/temp", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", String(fridge.temperature, 1));
  });

  // Full status JSON (use this for the dashboard)
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", buildStatusJSON());
  });

  // Set temperature threshold
  server.on("/setThreshold", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("value")) {
      tempThreshold = req->getParam("value")->value().toFloat();
      Serial.print("[Threshold] Temp set to: ");
      Serial.println(tempThreshold);
    }
    req->send(200, "text/plain", "OK");
  });

  // Set humidity threshold
  server.on("/setHumThreshold", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("value")) {
      humThreshold = req->getParam("value")->value().toFloat();
      Serial.print("[Threshold] Humidity set to: ");
      Serial.println(humThreshold);
    }
    req->send(200, "text/plain", "OK");
  });

  // Medicine log (stub — stores nothing, just acknowledges)
  server.on("/add", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", "OK");
  });

  // Reminder log (stub)
  server.on("/reminder", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", "OK");
  });

  server.begin();
  Serial.println("[Server] Started");

  // ── Init device state ──
  fridge.id          = "FRIDGE_001";
  fridge.temperature = 0.0;
  fridge.humidity    = 0.0;
  fridge.status      = "WAITING";
  fridge.sequence    = 0;
  fridge.rssi        = 0;
  fridge.online      = false;
  fridge.lastSeen    = 0;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Waiting LoRa...");
  Serial.println("=== HUB NODE READY ===\n");
}

// ================================================================
// LOOP
// ================================================================
void loop() {
  // ── Check for incoming LoRa packet ──
  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    String incoming = "";
    while (LoRa.available()) {
      incoming += (char)LoRa.read();
    }
    fridge.rssi = LoRa.packetRssi();

    Serial.println("--- Packet Received ---");
    Serial.print("Raw  : "); Serial.println(incoming);
    Serial.print("RSSI : "); Serial.println(fridge.rssi);

    if (parsePacket(incoming)) {
      Serial.print("ID   : "); Serial.println(fridge.id);
      Serial.print("Seq  : "); Serial.println(fridge.sequence);
      Serial.print("Temp : "); Serial.println(fridge.temperature);
      Serial.print("Hum  : "); Serial.println(fridge.humidity);
      Serial.print("Stat : "); Serial.println(fridge.status);
    } else {
      Serial.println("[Parser] Invalid packet format");
    }
    Serial.println();
  }

  // ── Offline detection ──
  if (fridge.lastSeen > 0 && millis() - fridge.lastSeen > 30000) {
    fridge.online = false;
  }

  // ── Alert logic ──
  tempAlert = (fridge.online && fridge.temperature > tempThreshold && fridge.temperature > -900);
  humAlert  = (fridge.online && fridge.humidity    > humThreshold  && fridge.humidity    > -900);

  // ── Buzzer ──
  if (tempAlert || humAlert) {
    // Beep pattern: 500ms on, 500ms off
    digitalWrite(BUZZER_PIN, (millis() % 1000) < 500 ? HIGH : LOW);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }

  // ── LCD Update (every 500ms, toggles between temp and humidity) ──
  if (millis() - lastLcdUpdate > 500) {
    lastLcdUpdate = millis();

    lcd.setCursor(0, 0);
    if (!fridge.online) {
      lcd.print("Fridge: OFFLINE ");
    } else if (fridge.temperature > -900) {
      lcd.print("T:");
      lcd.print(fridge.temperature, 1);
      lcd.print("C H:");
      lcd.print(fridge.humidity, 0);
      lcd.print("%  ");
    } else {
      lcd.print("Sensor Error    ");
    }

    lcd.setCursor(0, 1);
    if (!fridge.online && fridge.lastSeen == 0) {
      lcd.print("Waiting...      ");
    } else if (tempAlert) {
      lcd.print("! TEMP ALERT !  ");
    } else if (humAlert) {
      lcd.print("! HUM ALERT !   ");
    } else if (fridge.status == "OK") {
      lcd.print("Status: OK      ");
    } else {
      // Show sensor status on line 2
      String s = fridge.status;
      s.replace(",", " ");
      lcd.print(s.substring(0, 16));
    }
  }
}
```

---

## ⬆️ Upload to Hub ESP32

### Arduino IDE Settings

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module |
| Upload Speed | 115200 |
| Flash Size | 4MB (32Mb) |
| Partition Scheme | **Default 4MB with spiffs** |
| Port | Hub ESP32 COM port |

### Upload Steps

1. Click **Upload** in Arduino IDE
2. Hold **BOOT** when you see `Connecting.....`
3. Release BOOT once uploading starts
4. After `Done uploading` → open **Serial Monitor at 115200 baud**
5. Press **EN** button on ESP32

### Expected Startup in Serial Monitor

```
=== HUB NODE STARTING ===
[LCD] OK
[SPIFFS] OK
[WiFi] AP started: Medicine_Box | IP: 192.168.4.1
[LoRa] Initializing at 433 MHz... OK
[Server] Started
=== HUB NODE READY ===
```

If SPIFFS says no files, that's OK for now — the `index.html` upload is a separate step below.

---

## 📋 SPIFFS Dashboard Upload (Upload index.html)

The existing `index.html` dashboard needs to go into the ESP32's flash memory.

### Method — Arduino IDE ESP32 SPIFFS Uploader

1. Place `index.html` inside your Arduino sketch folder in a subfolder called `data`:
   ```
   Hub_Receiver_Server/
   ├── Hub_Receiver_Server.ino
   └── data/
       └── index.html
   ```
2. Copy the `index.html` from the project folder (`IOT PROJECT/index.html`) into `data/`
3. In Arduino IDE 2.x, SPIFFS upload is done via a plugin:
   - Download: https://github.com/me-no-dev/arduino-esp32fs-plugin/releases
   - Unzip into your Arduino IDE `tools/` folder
   - Restart Arduino IDE
   - You should see **Tools → ESP32 Sketch Data Upload**
4. Click **Tools → ESP32 Sketch Data Upload** (board must be selected, correct port set)
5. Wait for upload to complete

> **Alternative if the plugin doesn't work:** The hub code already has a fallback page that serves a plain status page at `/` and full JSON at `/status`. You can still test everything without the dashboard — the LoRa + LCD + buzzer all work independently.

---

## 🧪 Testing Guide — Step by Step

### Test 1 — Hub Standalone (No Fridge Node)

**Goal:** Confirm LCD, buzzer, WiFi, and LoRa all start up correctly.

1. Power the hub ESP32 via USB
2. Open Serial Monitor at 115200
3. Confirm you see `[LoRa] OK`, `[WiFi] AP started`, `[LCD] OK`
4. Look at the LCD — should show `Waiting LoRa...` on line 1
5. On your phone, go to **WiFi settings** → connect to `Medicine_Box` (password: `12345678`)
6. Open browser → go to `http://192.168.4.1`
7. You should see either the dashboard or the fallback "Hub Online" page
8. Open browser → go to `http://192.168.4.1/status`
9. Should return JSON like:
   ```json
   {"id":"FRIDGE_001","temperature":0.0,"humidity":0.0,"sequence":0,"rssi":0,"status":"WAITING","online":false,...}
   ```

**Pass criteria:**
- ✅ Serial Monitor shows all `OK` lines
- ✅ LCD shows `Waiting LoRa...`
- ✅ WiFi `Medicine_Box` visible on phone
- ✅ `/status` returns JSON

---

### Test 2 — LoRa Receive Test (With Fridge Node)

**Goal:** Confirm hub receives packets from fridge node.

1. Power both ESP32s
2. On the hub Serial Monitor, watch for:
   ```
   --- Packet Received ---
   Raw  : FRIDGE_001|1|23.50|52.30|OK
   RSSI : -45
   ID   : FRIDGE_001
   Seq  : 1
   Temp : 23.50
   Hum  : 52.30
   Stat : OK
   ```
3. Watch the LCD — should now show:
   - Line 1: `T:23.5C H:52%`
   - Line 2: `Status: OK`
4. On your phone connected to `Medicine_Box`, go to `http://192.168.4.1/status`
5. JSON should show real temperature and humidity values with `"online":true`

**Pass criteria:**
- ✅ Packets appear in Serial Monitor
- ✅ LCD updates with real values
- ✅ `/status` JSON shows `"online":true` and real readings
- ✅ Buzzer stays silent (room temp should be below 30°C threshold)

---

### Test 3 — Temperature Alert Test

**Goal:** Confirm LCD and buzzer trigger at threshold.

1. While both boards are running, open browser → `http://192.168.4.1/setThreshold?value=10`
2. This sets the threshold to 10°C — room temperature should now exceed it
3. Observe:
   - Line 2 of LCD should switch to `! TEMP ALERT !`
   - Buzzer should start beeping (500ms on/off pattern)
4. Reset threshold → `http://192.168.4.1/setThreshold?value=30`
5. Buzzer stops, LCD shows `Status: OK`

**Pass criteria:**
- ✅ Buzzer beeps when threshold breached
- ✅ LCD shows alert message
- ✅ Resets correctly when threshold raised back

---

### Test 4 — Offline Detection Test

**Goal:** Confirm hub detects when fridge node goes offline.

1. Power off the fridge ESP32 (unplug USB)
2. Wait 30 seconds
3. Check hub Serial Monitor — no more `Packet Received` lines
4. LCD line 1 should change to: `Fridge: OFFLINE`
5. Go to `/status` — `"online":false`

**Pass criteria:**
- ✅ LCD shows OFFLINE after 30 seconds
- ✅ JSON `"online":false`
- ✅ Buzzer stops (no false alerts when offline)

---

### Test 5 — Dashboard Full Test (if SPIFFS upload worked)

1. Phone connected to `Medicine_Box`
2. Go to `http://192.168.4.1`
3. Dashboard should load with real temperature
4. Temperature page should show live readings and the graph updating every 2 seconds
5. Try setting a threshold from the dashboard — verify buzzer/LCD respond

---

## 🚨 Troubleshooting

| Symptom | Fix |
|---|---|
| `[LoRa] FAILED!` at startup | Check SPI wiring; confirm 3.3V on RA-02 VCC; check antenna attached |
| LCD blank / no display | Check SDA=GPIO21, SCL=GPIO22; check I2C address is 0x27; try address 0x3F |
| LCD shows garbage | Check VCC — try 5V (Vin pin) instead of 3.3V for the LCD |
| No WiFi `Medicine_Box` visible | Upload failed; try re-uploading; check for compile errors |
| `/status` returns nothing | Wrong IP — confirm `192.168.4.1`; confirm connected to Medicine_Box not home WiFi |
| No packets from fridge | LoRa settings mismatch — confirm frequency=433MHz, SF=7, BW=125kHz on BOTH |
| Buzzer won't stop | Threshold too low — call `/setThreshold?value=30` from browser |
| SPIFFS upload option missing | Plugin not installed correctly — use the fallback page and manually test `/status` |
| `[SPIFFS] FAILED` | Usually harmless — hub still works for LoRa + LCD + buzzer, just no dashboard |

---

## 📡 LoRa Settings — Must Match Fridge Node

| Parameter | Hub Value | Fridge Value |
|---|---|---|
| Frequency | 433 MHz | 433 MHz |
| Spreading Factor | 7 | 7 |
| Bandwidth | 125 kHz | 125 kHz |
| Coding Rate | 4/5 | 4/5 |
| CRC | Enabled | Enabled |

---

## ✅ Final Pre-Demo Checklist

- [ ] Hub serial shows all `OK` lines on startup
- [ ] LCD powers on and shows text (not blank)
- [ ] WiFi AP `Medicine_Box` visible on phone
- [ ] `http://192.168.4.1/status` returns valid JSON
- [ ] Packets received from fridge node (LCD updates, JSON `"online":true`)
- [ ] Buzzer triggers when threshold lowered below room temperature
- [ ] Buzzer stops when threshold raised back to 30°C
- [ ] Offline detection works after 30s of no packets
- [ ] Dashboard loads on phone (if SPIFFS upload done)

---

## 📞 Quick API Reference (for demo / showing the panel)

| URL | What It Does |
|---|---|
| `http://192.168.4.1/` | Dashboard page |
| `http://192.168.4.1/status` | Full JSON status of fridge node |
| `http://192.168.4.1/temp` | Just the temperature as plain text |
| `http://192.168.4.1/setThreshold?value=25` | Set temp alert threshold to 25°C |
| `http://192.168.4.1/setHumThreshold?value=70` | Set humidity alert threshold to 70% |

---

*Smart Medicine Storage System — Hub Module Guide — October 2026*
*Fridge module guide: FRIDGE_MODULE_BUILD_GUIDE.md*
