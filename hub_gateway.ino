// ================================================================
//  HUB GATEWAY FIRMWARE v2 — Smart Medicine Storage System
//  Board    : ESP32 DevKit V1
//  Receives : LoRa packets from fridge nodes (SX1278 / RA-02, 433 MHz)
//  Serves   : Local dashboard (SPIFFS) + JSON API over WiFi AP ("Jwanil")
//  Cloud    : Pushes measurements to Firebase Realtime Database
//
//  WHAT CHANGED vs v1
//   * Radio is brought up FIRST (before WiFi) and runs in continuous RX.
//     v1 used LoRa.parsePacket() polling, which re-arms a single-shot
//     RX with a ~100 symbol timeout and was stalled for seconds by the
//     blocking Firebase TLS calls.
//   * Packets are read straight from the SX127x registers (IRQ flags),
//     so nothing depends on the DIO0 wire.
//   * "[RF]" health line every 5 s shows what the radio really sees:
//     RSSI floor/peak, preamble detected, header valid, CRC errors.
//     It tells you WHERE the link breaks (see bottom of this file).
//   * Strict packet validation (prefix + field count + value ranges).
//     Rejected frames are dumped as hex instead of silently ignored.
//   * Private sync word 0xF3 (must match fridge_node_v2.ino).
//   * Firebase pushes run in their own FreeRTOS task + queue, so the
//     radio loop is never blocked by HTTPS.
//   * Radio auto-recovers if the chip stops answering on SPI.
//   * New endpoint /radio returns the same diagnostics as JSON.
//   * Web handlers no longer race the loop on shared Strings (mutex).
//
//  PIN SUMMARY:
//    LoRa NSS -> GPIO 15, RST -> 14, DIO0 -> 26 (optional now)
//    LoRa SCK -> 18, MISO -> 19, MOSI -> 23
// ================================================================

#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>

// 1 = radio only (no WiFi, no web server, no cloud). Use this to prove
// the LoRa link in isolation from WiFi current spikes.
#define RADIO_ONLY_TEST   0
// 1 = print all 128 SX127x registers after setup (diff with the fridge's)
#define DUMP_REGISTERS    1

// ── LORA PINS ────────────────────────────────────────────────────
#define LORA_SS    15
#define LORA_RST   14
#define LORA_DIO0  26

// ── LORA RADIO SETTINGS — MUST BE IDENTICAL IN fridge_node_v2.ino ─
#define LORA_FREQUENCY  433E6
#define LORA_SF         7
#define LORA_BW         125E3
#define LORA_CR         5
#define LORA_PREAMBLE   12
#define LORA_SYNC_WORD  0xF3

// ── SX127x REGISTERS / BITS ──────────────────────────────────────
#define REG_FIFO              0x00
#define REG_OP_MODE           0x01
#define REG_FIFO_ADDR_PTR     0x0D
#define REG_FIFO_RX_CURRENT   0x10
#define REG_IRQ_FLAGS         0x12
#define REG_RX_NB_BYTES       0x13
#define REG_RX_HEADER_CNT     0x14   // 16-bit, valid headers since entering RX
#define REG_RX_PACKET_CNT     0x16   // 16-bit, valid packets since entering RX
#define REG_MODEM_STAT        0x18
#define REG_PKT_SNR           0x19
#define REG_PKT_RSSI          0x1A
#define REG_RSSI              0x1B
#define REG_FEI_MSB           0x28
#define REG_SYNC_WORD         0x39
#define REG_VERSION           0x42

#define IRQ_RX_DONE           0x40
#define IRQ_CRC_ERR           0x20

// ── WIFI — LOCAL ACCESS POINT ────────────────────────────────────
const char* AP_SSID     = "MedHub";   // hub's own WiFi (was "Jwanil", same name as the phone hotspot)
const char* AP_PASSWORD = "12345678";

// ── WIFI — INTERNET (for Firebase cloud sync) ────────────────────
const char* WIFI_SSID     = "Jwanil";
const char* WIFI_PASSWORD = "12345678";

// ── FIREBASE REALTIME DATABASE ───────────────────────────────────
const char* FIREBASE_HOST   = "iot-project-1dd40-default-rtdb.firebaseio.com";
const char* FIREBASE_SECRET = "";

// ── THRESHOLDS ───────────────────────────────────────────────────
float tempThreshold = 30.0;
float humThreshold  = 80.0;
volatile bool configDirty = false;   // set when thresholds are changed from the local dashboard

// ── DEVICE STATE ─────────────────────────────────────────────────
struct FridgeData {
  String   id           = "FRIDGE_001";
  float    temperature  = 0.0;
  float    humidity     = 0.0;
  String   sensorStatus = "WAITING";
  uint32_t sequence     = 0;
  int      rssi         = 0;
  float    snr          = 0;
  bool     online       = false;
  unsigned long lastSeen = 0;
};
FridgeData fridge;
SemaphoreHandle_t dataMutex = nullptr;   // guards `fridge`

// ── RADIO DIAGNOSTIC STATE ───────────────────────────────────────
bool     radioOk = false;
uint8_t  lastVersion = 0, lastOpMode = 0;
float    lastFeiHz = 0;
uint32_t rxOk = 0, rxCrcErr = 0, rxRejected = 0, radioReinits = 0;
// per 5-second window
uint32_t smpTotal = 0, smpDetect = 0, smpSync = 0, smpHeader = 0;
int      rssiMin = 0, rssiMax = -200;

// ── WEB SERVER / TIMING / ALERTS ─────────────────────────────────
AsyncWebServer server(80);
const unsigned long OFFLINE_TIMEOUT = 30000;
bool tempAlert = false;
bool humAlert  = false;

// ── CLOUD QUEUE ──────────────────────────────────────────────────
struct CloudJob { float t; float h; uint32_t seq; };
QueueHandle_t cloudQueue = nullptr;

// ================================================================
//  RAW SPI REGISTER ACCESS
//  Only ever called from loop()/setup() (never from web handlers).
// ================================================================
static const SPISettings loraSpi(1000000, MSBFIRST, SPI_MODE0);   // 1 MHz: tolerant of jumper wires

uint8_t loraRead(uint8_t reg) {
  SPI.beginTransaction(loraSpi);
  digitalWrite(LORA_SS, LOW);
  SPI.transfer(reg & 0x7F);
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(LORA_SS, HIGH);
  SPI.endTransaction();
  return v;
}

void loraWrite(uint8_t reg, uint8_t val) {
  SPI.beginTransaction(loraSpi);
  digitalWrite(LORA_SS, LOW);
  SPI.transfer(reg | 0x80);
  SPI.transfer(val);
  digitalWrite(LORA_SS, HIGH);
  SPI.endTransaction();
}

uint16_t loraRead16(uint8_t regMsb) {
  return ((uint16_t)loraRead(regMsb) << 8) | loraRead(regMsb + 1);
}

// Garbage-frame tracking: bursts of CRC errors / impossible frames mean the
// chip or the SPI bus is misbehaving (supply droop, loose wire) -> hard reset.
uint32_t glitchCount = 0;
unsigned long glitchWindowStart = 0;
void noteGlitch() {
  unsigned long now = millis();
  if (now - glitchWindowStart > 10000) { glitchWindowStart = now; glitchCount = 0; }
  glitchCount++;
}

// ================================================================
//  RADIO INIT (also used for recovery) — leaves chip in continuous RX
// ================================================================
bool initRadio() {
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  pinMode(LORA_RST, OUTPUT);
  digitalWrite(LORA_RST, LOW);  delay(100);
  digitalWrite(LORA_RST, HIGH); delay(200);

  pinMode(LORA_SS, OUTPUT);
  digitalWrite(LORA_SS, HIGH);
  LoRa.setSPIFrequency(1000000);

  // The LoRa library divides by values it reads from the chip and crashes
  // (IntegerDivideByZero) if the bus returns garbage. Check first.
  for (int i = 0; i < 3; i++) {
    uint8_t v = loraRead(REG_VERSION);
    if (v != 0x12) {
      Serial.printf("bus check failed (version read 0x%02X) ", v);
      return false;
    }
    delay(5);
  }

  bool ok = false;
  for (int i = 1; i <= 5; i++) {
    if (LoRa.begin(LORA_FREQUENCY)) { ok = true; break; }
    Serial.print(F("retry ")); Serial.print(i); Serial.print(F("... "));
    delay(500);
  }
  if (!ok) return false;

  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setPreambleLength(LORA_PREAMBLE);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  LoRa.receive();                       // continuous RX
  return true;
}

void printRadioReadback() {
  uint32_t frf = ((uint32_t)loraRead(0x06) << 16) | ((uint32_t)loraRead(0x07) << 8) | loraRead(0x08);
  float freqMHz = frf * (32.0f / 524288.0f);
  Serial.printf("[LoRa] version=0x%02X  freq=%.3f MHz  modemCfg1=0x%02X  modemCfg2=0x%02X  modemCfg3=0x%02X\n",
                loraRead(REG_VERSION), freqMHz, loraRead(0x1D), loraRead(0x1E), loraRead(0x26));
  Serial.printf("[LoRa] syncWord=0x%02X  opMode=0x%02X (0x85 = continuous RX)\n",
                loraRead(REG_SYNC_WORD), loraRead(REG_OP_MODE));
}

// ================================================================
//  PACKET PARSER (strict)
//  Format: FRIDGE_xxx|SEQ|TEMP_C|HUMIDITY|STATUS
//  -999 in TEMP/HUM is the fridge's "sensor error" marker.
// ================================================================
bool parseFridgePacket(const String& raw, int rssi, float snr, String& why) {
  if (!raw.startsWith("FRIDGE_")) { why = "does not start with FRIDGE_"; return false; }

  int p[4];
  int from = 0;
  for (int i = 0; i < 4; i++) {
    p[i] = raw.indexOf('|', from);
    if (p[i] < 0) { why = "fewer than 5 fields"; return false; }
    from = p[i] + 1;
  }

  String id     = raw.substring(0, p[0]);
  long   seq    = raw.substring(p[0] + 1, p[1]).toInt();
  float  t      = raw.substring(p[1] + 1, p[2]).toFloat();
  float  h      = raw.substring(p[2] + 1, p[3]).toFloat();
  String status = raw.substring(p[3] + 1);
  status.trim();

  bool tOk = (t > -60.0f && t < 130.0f) || t < -900.0f;
  bool hOk = (h >= 0.0f && h <= 100.0f) || h < -900.0f;
  if (!tOk || !hOk)                          { why = "temperature/humidity out of range"; return false; }
  if (status.length() == 0 || status.length() > 40) { why = "bad status field"; return false; }
  if (id.length() > 20)                      { why = "device id too long"; return false; }

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  fridge.id           = id;
  fridge.sequence     = (uint32_t)seq;
  fridge.temperature  = t;
  fridge.humidity     = h;
  fridge.sensorStatus = status;
  fridge.rssi         = rssi;
  fridge.snr          = snr;
  fridge.lastSeen     = millis();
  fridge.online       = true;
  xSemaphoreGive(dataMutex);
  return true;
}

// ================================================================
//  JSON BUILDERS
// ================================================================
String buildStatusJSON() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  if (fridge.lastSeen > 0 && (int32_t)(millis() - fridge.lastSeen) > (int32_t)OFFLINE_TIMEOUT) {
    fridge.online = false;
  }

  String j = "{";
  j += "\"id\":\""          + fridge.id + "\",";
  j += "\"temperature\":"   + String(fridge.temperature, 1) + ",";
  j += "\"humidity\":"      + String(fridge.humidity, 1) + ",";
  j += "\"sequence\":"      + String(fridge.sequence) + ",";
  j += "\"rssi\":"          + String(fridge.rssi) + ",";
  j += "\"sensorStatus\":\"" + fridge.sensorStatus + "\",";
  j += "\"online\":"        + String(fridge.online ? "true" : "false") + ",";
  j += "\"tempThreshold\":" + String(tempThreshold, 1) + ",";
  j += "\"humThreshold\":"  + String(humThreshold, 1) + ",";
  j += "\"tempAlert\":"     + String(tempAlert  ? "true" : "false") + ",";
  j += "\"humAlert\":"      + String(humAlert   ? "true" : "false") + ",";
  j += "\"lastSeen\":"      + String(fridge.lastSeen);
  j += "}";
  xSemaphoreGive(dataMutex);
  return j;
}

// Cached values only — never touches SPI (safe from the web task).
String buildRadioJSON() {
  String j = "{";
  j += "\"radioOk\":"      + String(radioOk ? "true" : "false") + ",";
  j += "\"version\":"      + String(lastVersion) + ",";
  j += "\"opMode\":"       + String(lastOpMode) + ",";
  j += "\"rxOk\":"         + String(rxOk) + ",";
  j += "\"rxCrcErr\":"     + String(rxCrcErr) + ",";
  j += "\"rxRejected\":"   + String(rxRejected) + ",";
  j += "\"reinits\":"      + String(radioReinits) + ",";
  j += "\"lastFeiHz\":"    + String(lastFeiHz, 0) + ",";
  j += "\"rssiMin\":"      + String(rssiMin) + ",";
  j += "\"rssiMax\":"      + String(rssiMax);
  j += "}";
  return j;
}

// ================================================================
//  FIREBASE CLOUD SYNC — runs in its own task, never blocks the radio
// ================================================================
// ── Threshold sync between cloud dashboard and hub ───────────────
// The cloud dashboard writes thresholds to /config/FRIDGE_001. Each cycle the hub either
// pushes its own thresholds there (if changed on the local dashboard) or adopts the cloud's.
bool jsonNumber(const String& s, const char* key, float& out) {
  String k = String("\"") + key + "\":";
  int i = s.indexOf(k);
  if (i < 0) return false;
  out = s.substring(i + k.length()).toFloat();
  return true;
}

void syncConfig(WiFiClientSecure& client, const String& baseUrl, const String& auth) {
  HTTPClient http;
  http.setTimeout(8000);
  String url = baseUrl + "/config/FRIDGE_001.json" + auth;

  if (configDirty) {
    configDirty = false;
    String body = String("{\"tempThreshold\":") + String(tempThreshold, 1) +
                  ",\"humThreshold\":" + String(humThreshold, 1) + "}";
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    int code = http.PATCH(body);
    Serial.printf("[Firebase] Config PATCH -> %d\n", code);
    http.end();
    return;
  }

  http.begin(client, url);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    float v;
    if (jsonNumber(payload, "tempThreshold", v) && v >= 0 && v <= 60  && v != tempThreshold) {
      tempThreshold = v; Serial.printf("[Config] Temp threshold from cloud = %.1f\n", v);
    }
    if (jsonNumber(payload, "humThreshold", v) && v >= 0 && v <= 100 && v != humThreshold) {
      humThreshold = v;  Serial.printf("[Config] Hum threshold from cloud = %.1f\n", v);
    }
  }
  http.end();
}

void pushToFirebase(const CloudJob& job) {
  if (strlen(FIREBASE_HOST) == 0) return;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[Firebase] WiFi not connected — skipping cloud push"));
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  String baseUrl = String("https://") + FIREBASE_HOST;
  String auth = (strlen(FIREBASE_SECRET) > 0) ? String("?auth=") + FIREBASE_SECRET : "";

  // 0. Threshold sync (cloud <-> hub)
  syncConfig(client, baseUrl, auth);

  // 1. Live device state
  {
    HTTPClient http;
    http.setTimeout(8000);
    http.begin(client, baseUrl + "/devices/FRIDGE_001.json" + auth);
    http.addHeader("Content-Type", "application/json");
    String live = buildStatusJSON();
    live.remove(live.length() - 1);                        // drop the closing }
    live += ",\"lastSeenTs\":{\".sv\":\"timestamp\"}}";   // Firebase fills in real epoch ms
    int code = http.PUT(live);
    if (code > 0) { Serial.print(F("[Firebase] Live State PUT -> ")); Serial.print(code); }
    else          { Serial.print(F("[Firebase] Live State Error: ")); Serial.print(http.errorToString(code)); }
    http.end();
  }

  // 2. History entry
  if (job.t > -900 || job.h > -900) {
    String hist = "{";
    hist += "\"t\":"   + String(job.t, 2) + ",";
    hist += "\"h\":"   + String(job.h, 2) + ",";
    hist += "\"seq\":" + String(job.seq) + ",";
    hist += "\"ts\":{\".sv\":\"timestamp\"}";
    hist += "}";

    HTTPClient http2;
    http2.setTimeout(8000);
    http2.begin(client, baseUrl + "/history/FRIDGE_001.json" + auth);
    http2.addHeader("Content-Type", "application/json");
    int hcode = http2.POST(hist);
    if (hcode > 0) { Serial.print(F(" | History POST -> ")); Serial.println(hcode); }
    else           { Serial.print(F(" | History Error: ")); Serial.println(http2.errorToString(hcode)); }
    http2.end();
  } else {
    Serial.println();
  }
}

void cloudTask(void*) {
  CloudJob job;
  for (;;) {
    if (xQueueReceive(cloudQueue, &job, portMAX_DELAY) == pdTRUE) {
      pushToFirebase(job);
    }
  }
}

// ================================================================
//  PACKET HANDLING
// ================================================================
void handleRxPacket(const uint8_t* buf, uint8_t len, int rssi, float snr, float feiHz) {
  String raw, printable;
  raw.reserve(len);
  printable.reserve(len);
  for (uint8_t i = 0; i < len; i++) {
    raw += (char)buf[i];
    printable += (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.';
  }

  Serial.println(F("\n=================================="));
  Serial.println(F("   >>> LORA PACKET RECEIVED <<<"));
  Serial.printf("Bytes: %u | RSSI: %d dBm | SNR: %.2f dB | FreqErr: %.0f Hz\n",
                (unsigned)len, rssi, snr, feiHz);
  Serial.print(F("Raw  : ")); Serial.println(printable);

  String why;
  if (parseFridgePacket(raw, rssi, snr, why)) {
    rxOk++;
    Serial.print(F("Device: ")); Serial.println(fridge.id);
    Serial.print(F("Seq   : ")); Serial.println(fridge.sequence);
    Serial.print(F("Temp  : ")); Serial.print(fridge.temperature); Serial.println(F(" C"));
    Serial.print(F("Hum   : ")); Serial.print(fridge.humidity);    Serial.println(F(" %"));
    Serial.print(F("Status: ")); Serial.println(fridge.sensorStatus);

#if !RADIO_ONLY_TEST
    if (cloudQueue) {
      CloudJob job = { fridge.temperature, fridge.humidity, fridge.sequence };
      if (xQueueSend(cloudQueue, &job, 0) != pdTRUE) {
        Serial.println(F("[Firebase] queue full — dropping this sample"));
      }
    }
#endif
  } else {
    rxRejected++;
    noteGlitch();
    Serial.print(F("[Parser] REJECTED: ")); Serial.println(why);
    Serial.print(F("Hex  : "));
    for (uint8_t i = 0; i < len && i < 48; i++) Serial.printf("%02X ", buf[i]);
    Serial.println(len > 48 ? F("...") : F(""));
  }
  Serial.println(F("==================================\n"));
}

// Called every loop iteration (~5 ms). Samples RF state and drains RxDone.
void pollRadio() {
  // --- live RF sampling for the health line ---
  uint8_t stat = loraRead(REG_MODEM_STAT);
  smpTotal++;
  if (stat & 0x01) smpDetect++;      // preamble / signal detected
  if (stat & 0x02) smpSync++;        // signal synchronized
  if (stat & 0x08) smpHeader++;      // header info valid
  int rssiNow = -164 + (int)loraRead(REG_RSSI);
  if (rssiNow > rssiMax) rssiMax = rssiNow;
  if (rssiNow < rssiMin) rssiMin = rssiNow;

  // --- packet finished? ---
  uint8_t irq = loraRead(REG_IRQ_FLAGS);
  if (!(irq & IRQ_RX_DONE)) return;

  loraWrite(REG_IRQ_FLAGS, 0xFF);    // clear all flags (write 1 to clear)

  // Frequency error of this packet (20-bit signed), in Hz
  int32_t fei = ((int32_t)(loraRead(REG_FEI_MSB) & 0x0F) << 16)
              | ((int32_t)loraRead(REG_FEI_MSB + 1) << 8)
              |  (int32_t)loraRead(REG_FEI_MSB + 2);
  if (fei & 0x80000) fei -= 0x100000;
  lastFeiHz = fei * 0.524288f * (LORA_BW / 1000.0f) / 500.0f;

  int8_t snrRaw = (int8_t)loraRead(REG_PKT_SNR);
  float  snr    = snrRaw * 0.25f;
  int    rssi   = -164 + (int)loraRead(REG_PKT_RSSI);
  if (snr < 0) rssi += (int)snr;

  if (irq & IRQ_CRC_ERR) {
    rxCrcErr++;
    noteGlitch();
    Serial.printf("[RF] Frame with PAYLOAD CRC ERROR (rssi %d dBm, snr %.1f dB, freqErr %.0f Hz) — header was valid, payload corrupted\n",
                  rssi, snr, lastFeiHz);
    return;
  }

  uint8_t len = loraRead(REG_RX_NB_BYTES);
  if (len == 0 || len > 80) {            // real packets are ~40 bytes
    noteGlitch();
    Serial.printf("[RF] Impossible frame length %u — ignoring (chip/SPI glitch)\n", (unsigned)len);
    return;
  }
  uint8_t cur = loraRead(REG_FIFO_RX_CURRENT);
  loraWrite(REG_FIFO_ADDR_PTR, cur);

  static uint8_t buf[256];
  for (uint16_t i = 0; i < len; i++) buf[i] = loraRead(REG_FIFO);

  handleRxPacket(buf, len, rssi, snr, lastFeiHz);
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n=================================="));
  Serial.println(F("   HUB GATEWAY v2 STARTING (LoRa RX)"));
  Serial.println(F("=================================="));

  dataMutex = xSemaphoreCreateMutex();

  // ── LoRa FIRST, before WiFi ──
  SPI.begin(18, 19, 23, LORA_SS);
  Serial.print(F("[LoRa] Starting at 433 MHz (NSS=GPIO ")); Serial.print(LORA_SS); Serial.print(F(")... "));
  radioOk = initRadio();
  if (radioOk) {
    Serial.println(F("OK"));
    printRadioReadback();
#if DUMP_REGISTERS
    Serial.println(F("[LoRa] Register dump:"));
    LoRa.dumpRegisters(Serial);
#endif
  } else {
    Serial.println(F("FAILED — will keep retrying every 5 s while the dashboard stays up"));
    Serial.println(F("Check: 3.3V power, antenna, SPI pins (NSS=15,RST=14,DIO0=26,SCK=18,MISO=19,MOSI=23)"));
  }

#if !RADIO_ONLY_TEST
  // ── SPIFFS ──
  if (!SPIFFS.begin(true)) {
    Serial.println(F("[SPIFFS] Mount failed — local dashboard unavailable"));
  } else {
    Serial.println(F("[SPIFFS] OK"));
    File root = SPIFFS.open("/");
    File f    = root.openNextFile();
    while (f) {
      Serial.print(F("  - ")); Serial.println(f.name());
      f = root.openNextFile();
    }
  }

  // ── WiFi ──
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print(F("[AP] Started: ")); Serial.print(AP_SSID);
  Serial.print(F(" | Local IP: ")); Serial.println(WiFi.softAPIP());

  if (strlen(WIFI_SSID) > 0) {
    Serial.print(F("[WiFi] Connecting to ")); Serial.print(WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 8000) {
      delay(500); Serial.print('.');
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println();
      Serial.print(F("[WiFi] Connected | Internet IP: ")); Serial.println(WiFi.localIP());
    } else {
      Serial.println(F("\n[WiFi] Could not connect to internet WiFi — running local-only"));
    }
  }

  // ── Cloud task ──
  cloudQueue = xQueueCreate(8, sizeof(CloudJob));
  xTaskCreatePinnedToCore(cloudTask, "cloud", 12288, nullptr, 1, nullptr, 0);

  // ── Web Server Routes ──
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (SPIFFS.exists("/index.html"))
      req->send(SPIFFS, "/index.html", "text/html");
    else
      req->send(200, "text/html",
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width'>"
        "<style>body{background:#0f1117;color:#e4e6ed;font-family:sans-serif;padding:20px}"
        "h2{color:#a29bfe}a{color:#74b9ff}</style></head><body>"
        "<h2>Medicine Hub Online</h2>"
        "<p>Local SPIFFS dashboard not uploaded yet.</p>"
        "<p><a href='/status'>View JSON status &rarr;</a></p>"
        "<p><a href='/radio'>View radio diagnostics &rarr;</a></p>"
        "</body></html>");
  });

  server.on("/temp", HTTP_GET, [](AsyncWebServerRequest* req) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    String s = String(fridge.temperature, 1);
    xSemaphoreGive(dataMutex);
    req->send(200, "text/plain", s);
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", buildStatusJSON());
  });

  server.on("/radio", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", buildRadioJSON());
  });

  server.on("/setThreshold", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("value")) {
      tempThreshold = req->getParam("value")->value().toFloat();
      configDirty = true;
      Serial.print(F("[Threshold] Temp = ")); Serial.println(tempThreshold);
    }
    req->send(200, "text/plain", "OK");
  });

  server.on("/setHumThreshold", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("value")) {
      humThreshold = req->getParam("value")->value().toFloat();
      configDirty = true;
      Serial.print(F("[Threshold] Hum = ")); Serial.println(humThreshold);
    }
    req->send(200, "text/plain", "OK");
  });

  server.on("/add",      HTTP_GET, [](AsyncWebServerRequest* req){ req->send(200,"text/plain","OK"); });
  server.on("/reminder", HTTP_GET, [](AsyncWebServerRequest* req){ req->send(200,"text/plain","OK"); });

  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println(F("[Server] HTTP Server Started"));
#endif

  Serial.println(F("=== HUB GATEWAY READY — listening for LoRa packets ===\n"));
}

// ================================================================
//  LOOP
// ================================================================
void loop() {
  unsigned long now = millis();
  static unsigned long lastHealth = 0;
  static unsigned long lastRadioRetry = 0;

  // ── Radio ──
  if (radioOk) {
    pollRadio();
  } else if (now - lastRadioRetry >= 5000) {
    lastRadioRetry = now;
    Serial.print(F("[LoRa] Retrying radio init... "));
    radioOk = initRadio();
    Serial.println(radioOk ? F("OK") : F("still failing"));
    if (radioOk) printRadioReadback();
  }

  // ── Burst of garbage frames -> hard reset of the radio ──
  if (radioOk && glitchCount >= 4) {
    Serial.println(F("[RF] Repeated garbage frames — hard-resetting radio"));
    glitchCount = 0;
    radioReinits++;
    radioOk = initRadio();
    if (radioOk) printRadioReadback();
  }

  // ── Health report every 5 s (= one fridge TX interval) ──
  if (now - lastHealth >= 5000) {
    lastHealth = now;

    if (radioOk) {
      lastVersion = loraRead(REG_VERSION);
      lastOpMode  = loraRead(REG_OP_MODE);

      if (lastVersion != 0x12) {
        Serial.printf("[RF] Chip stopped answering (version=0x%02X) — re-initialising\n", lastVersion);
        radioReinits++;
        radioOk = initRadio();
      } else if ((lastOpMode & 0x87) != 0x85) {
        Serial.printf("[RF] Chip left continuous RX (opMode=0x%02X) — re-entering RX\n", lastOpMode);
        radioReinits++;
        LoRa.receive();
      }
    }

    uint16_t hwHdr = radioOk ? loraRead16(REG_RX_HEADER_CNT) : 0;
    uint16_t hwPkt = radioOk ? loraRead16(REG_RX_PACKET_CNT) : 0;

    Serial.printf("[RF] ver=0x%02X mode=0x%02X | 5s: samples=%lu detect=%lu sync=%lu hdr=%lu | RSSI min/max=%d/%d dBm | RX ok=%lu crcErr=%lu rejected=%lu | hw hdr=%u pkt=%u | reinit=%lu\n",
                  lastVersion, lastOpMode,
                  (unsigned long)smpTotal, (unsigned long)smpDetect, (unsigned long)smpSync, (unsigned long)smpHeader,
                  rssiMin, rssiMax,
                  (unsigned long)rxOk, (unsigned long)rxCrcErr, (unsigned long)rxRejected,
                  (unsigned)hwHdr, (unsigned)hwPkt, (unsigned long)radioReinits);

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool    on  = fridge.online;
    float   tt  = fridge.temperature, hh = fridge.humidity;
    int     rs  = fridge.rssi;
    xSemaphoreGive(dataMutex);

    Serial.print(F("[Hub] "));
    if (on) {
      Serial.printf("FRIDGE_001 Online | Temp: %.2f C | Hum: %.1f %% | RSSI: %d dBm", tt, hh, rs);
    } else {
      Serial.print(F("Waiting for LoRa packets from fridge node..."));
    }
#if !RADIO_ONLY_TEST
    Serial.print(F(" | WiFi: "));
    Serial.print(WiFi.status() == WL_CONNECTED ? "Connected (" + WiFi.localIP().toString() + ")" : "AP Only (192.168.4.1)");
#endif
    Serial.println();

    // reset 5 s window
    smpTotal = smpDetect = smpSync = smpHeader = 0;
    rssiMin = 0; rssiMax = -200;
  }

  // ── Offline detection ──
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  // signed compare: a packet handled earlier in this same pass has lastSeen > now, which
  // used to wrap around as unsigned and mark the fridge offline right after every packet
  if (fridge.lastSeen > 0 && (int32_t)(millis() - fridge.lastSeen) > (int32_t)OFFLINE_TIMEOUT) fridge.online = false;
  bool  on = fridge.online;
  float tt = fridge.temperature, hh = fridge.humidity;
  xSemaphoreGive(dataMutex);

  // ── Alert evaluation ──
  tempAlert = on && (tt > tempThreshold) && (tt > -900);
  humAlert  = on && (hh > humThreshold)  && (hh > -900);

  delay(5);   // ~200 RF samples/s; yields to WiFi / FreeRTOS
}

// ================================================================
//  HOW TO READ THE "[RF]" LINE (fridge transmits once per 5 s window)
//
//  A) RSSI max stays near the noise floor (about -105 .. -120 dBm) and
//     detect=0 in every window
//       -> no RF from the fridge reaches the hub. Suspect the fridge TX
//          path: antenna/pigtail, PA pin (set LORA_USE_PA_BOOST 0 on the
//          fridge), supply droop (add 100 uF + 100 nF at RA-02 VCC).
//          Swap the two boards' firmware to see whether the fault
//          follows the module or the position.
//
//  B) RSSI max spikes once per window (say -60 .. -10 dBm) but detect=0
//       -> energy arrives but the modem cannot lock. Either the signal
//          is so strong the front end is overloaded (separate boards
//          3-5 m, lower LORA_TX_POWER) or modem settings differ: diff the
//          two register dumps (0x06-0x08, 0x1D, 0x1E, 0x26, 0x39).
//
//  C) detect/sync/hdr > 0 and crcErr increments
//       -> link works but the payload arrives corrupted: noisy supply
//          on the transmitter, or frequency error: look at FreqErr.
//          Above ~ +/-15 kHz points at a bad crystal.
//
//  D) RX ok = 0 but rejected > 0
//       -> valid LoRa frames arrive but are not the fridge's; the hex
//          dump shows what they are.
//
//  E) RX ok increments
//       -> link is fixed. Check Firebase lines next.
// ================================================================
