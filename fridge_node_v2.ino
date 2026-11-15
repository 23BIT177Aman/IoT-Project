// ================================================================
//  FRIDGE NODE FIRMWARE v2 — Smart Medicine Storage System
//  Board    : ESP32 DevKit V1
//  Sensors  : DS18B20 (temperature) + DHT22 (humidity)
//  Radio    : SX1278 / RA-02 LoRa at 433 MHz via SPI
//  Output   : Pipe-delimited LoRa packet every 5 seconds
//
//  Packet format (unchanged): FRIDGE_001|SEQ|TEMP_C|HUMIDITY|STATUS
//
//  WHAT CHANGED vs v1
//   * Private sync word (0xF3) on both nodes -> ignores other people's
//     LoRa traffic on 433 MHz (the hub used to decode foreign frames).
//   * Explicit preamble length (12) and every radio parameter written
//     explicitly, then read back from the chip and printed.
//   * TX power default lowered to 10 dBm for 1-3 m bench tests.
//   * Measures real on-air time. SF7/125k/CR4-5 with a ~31 byte payload
//     must take ~75 ms. endPacket() always "succeeds", so this is the
//     only honest sign the modem really transmitted.
//   * Re-initialises the radio if the chip stops answering on SPI
//     (brown-out / loose wire) instead of silently transmitting nothing.
//   * LED_BUILTIN is now configured as output (blinks on every TX).
//   * PA_BOOST / RFO output-pin switch (see LORA_USE_PA_BOOST).
//   * Optional full register dump for diffing against the hub.
//
//  PIN SUMMARY:
//    DS18B20 DATA  -> GPIO 4  (+ 4.7k pull-up to 3.3V)
//    DHT22 DATA    -> GPIO 16 (+ 10k pull-up if bare sensor)
//    LoRa NSS      -> GPIO 15
//    LoRa RST      -> GPIO 14
//    LoRa DIO0     -> GPIO 26
//    LoRa SCK/MISO/MOSI -> GPIO 18 / 19 / 23
//
//  HARDWARE TIP: solder or plug a 100 uF electrolytic + 100 nF ceramic
//  directly across the RA-02 VCC/GND pins. TX bursts draw 50-120 mA and
//  a drooping 3.3 V rail corrupts the transmitted frame.
// ================================================================

#include <SPI.h>
#include <LoRa.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>

// ── PIN DEFINITIONS ──────────────────────────────────────────────
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

#define ONE_WIRE_BUS  4
#define DHT_PIN       16
#define DHT_TYPE      DHT22

#define LORA_SS       15
#define LORA_RST      14
#define LORA_DIO0     26

// ── DEVICE IDENTITY ──────────────────────────────────────────────
const char* DEVICE_ID = "FRIDGE_001";

// ── LORA RADIO SETTINGS — MUST BE IDENTICAL IN hub_gateway_v2.ino ─
#define LORA_FREQUENCY   433E6
#define LORA_SF          7
#define LORA_BW          125E3
#define LORA_CR          5          // 4/5
#define LORA_PREAMBLE    12
#define LORA_SYNC_WORD   0xF3       // private network (default was 0x12)

// TX power in dBm. 10 is plenty for a bench test at 1-3 m; raise to
// 17 once the link works and the nodes are far apart.
#define LORA_TX_POWER    10
// RA-02 modules use the PA_BOOST pin. If the hub never sees a single
// RF spike (rssiMax stays near the noise floor) try 0 = RFO pin.
#define LORA_USE_PA_BOOST 1

// 1 = print all 128 SX127x registers after setup (diff with the hub's)
#define DUMP_REGISTERS   1

// ── TIMING ───────────────────────────────────────────────────────
const unsigned long SEND_INTERVAL = 5000;
unsigned long lastSend = 0;
uint32_t sequence = 0;

// ── SENSOR OBJECTS ───────────────────────────────────────────────
OneWire           oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);
DHT               dht(DHT_PIN, DHT_TYPE);

// ── RAW SPI REGISTER ACCESS (diagnostics only) ───────────────────
static const SPISettings loraSpi(4000000, MSBFIRST, SPI_MODE0);

uint8_t loraRead(uint8_t reg) {
  SPI.beginTransaction(loraSpi);
  digitalWrite(LORA_SS, LOW);
  SPI.transfer(reg & 0x7F);
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(LORA_SS, HIGH);
  SPI.endTransaction();
  return v;
}

// ================================================================
//  RADIO INIT (also used for recovery)
// ================================================================
bool initRadio() {
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  pinMode(LORA_RST, OUTPUT);
  digitalWrite(LORA_RST, LOW);  delay(100);
  digitalWrite(LORA_RST, HIGH); delay(200);

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
  LoRa.setTxPower(LORA_TX_POWER,
                  LORA_USE_PA_BOOST ? PA_OUTPUT_PA_BOOST_PIN : PA_OUTPUT_RFO_PIN);
  LoRa.enableCrc();
  return true;
}

void printRadioReadback() {
  // Values as the chip itself reports them
  uint32_t frf = ((uint32_t)loraRead(0x06) << 16) | ((uint32_t)loraRead(0x07) << 8) | loraRead(0x08);
  float freqMHz = frf * (32.0f / 524288.0f);          // Fxtal 32 MHz / 2^19
  Serial.printf("[LoRa] version=0x%02X  freq=%.3f MHz  modemCfg1=0x%02X  modemCfg2=0x%02X  modemCfg3=0x%02X\n",
                loraRead(0x42), freqMHz, loraRead(0x1D), loraRead(0x1E), loraRead(0x26));
  Serial.printf("[LoRa] syncWord=0x%02X  paConfig=0x%02X  paDac=0x%02X  opMode=0x%02X\n",
                loraRead(0x39), loraRead(0x09), loraRead(0x4D), loraRead(0x01));
}

// ================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.println(F("\n=== FRIDGE NODE v2 STARTING ==="));
  Serial.print(F("Device ID: ")); Serial.println(DEVICE_ID);

  ds18b20.begin();
  Serial.println(F("[DS18B20] Initialized"));

  dht.begin();
  delay(2000);
  Serial.println(F("[DHT22] Initialized"));

  SPI.begin(18, 19, 23, LORA_SS);  // SCK, MISO, MOSI, SS

  Serial.print(F("[LoRa] Starting at 433 MHz... "));
  if (!initRadio()) {
    Serial.println(F("FAILED!"));
    Serial.println(F("Check: VCC=3.3V, antenna attached, SPI pins correct."));
    while (true) {
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
      delay(200);
    }
  }
  Serial.println(F("OK"));
  printRadioReadback();
#if DUMP_REGISTERS
  Serial.println(F("[LoRa] Register dump:"));
  LoRa.dumpRegisters(Serial);
#endif
  Serial.println(F("=== FRIDGE NODE READY — sending every 5s ===\n"));
}

// ================================================================
void loop() {
  if (millis() - lastSend >= SEND_INTERVAL) {
    lastSend = millis();
    sendPacket();
  }
}

// ================================================================
void sendPacket() {
  // Is the radio still alive on SPI? (brown-out / loose wire recovery)
  if (loraRead(0x42) != 0x12) {
    Serial.println(F("[LoRa] Chip not answering (version != 0x12) — re-initialising"));
    if (!initRadio()) {
      Serial.println(F("[LoRa] Re-init FAILED — skipping this transmission"));
      return;
    }
    printRadioReadback();
  }

  // ── Read DS18B20 ──
  ds18b20.requestTemperatures();
  float tempC = ds18b20.getTempCByIndex(0);
  bool  tempOK = (tempC != DEVICE_DISCONNECTED_C && tempC > -55.0f && tempC < 125.0f);

  // ── Read DHT22 ──
  float humidity = dht.readHumidity();
  bool  humOK    = (!isnan(humidity) && humidity >= 0.0f && humidity <= 100.0f);

  sequence++;

  // ── Build status ──
  String status = "OK";
  if (!tempOK && !humOK) status = "DS18B20_ERR,DHT22_ERR";
  else if (!tempOK)      status = "DS18B20_ERR";
  else if (!humOK)       status = "DHT22_ERR";

  // ── Serial debug ──
  Serial.println(F("─── TX ───────────────────"));
  Serial.print(F("Seq     : ")); Serial.println(sequence);
  Serial.print(F("DS18B20 : "));
  if (tempOK) { Serial.print(tempC, 2); Serial.println(F(" C")); }
  else          Serial.println(F("ERROR"));
  Serial.print(F("DHT22 H : "));
  if (humOK)  { Serial.print(humidity, 1); Serial.println(F(" %")); }
  else          Serial.println(F("ERROR"));
  Serial.print(F("Status  : ")); Serial.println(status);

  // ── Build packet ──
  String pkt = String(DEVICE_ID)
             + "|" + sequence
             + "|" + String(tempOK ? tempC    : -999.0f, 2)
             + "|" + String(humOK  ? humidity : -999.0f, 2)
             + "|" + status;

  // ── Transmit (blocks until the modem reports TxDone) ──
  digitalWrite(LED_BUILTIN, HIGH);
  uint32_t t0 = millis();
  LoRa.beginPacket();
  LoRa.print(pkt);
  LoRa.endPacket();
  uint32_t airMs = millis() - t0;
  digitalWrite(LED_BUILTIN, LOW);

  Serial.printf("[TX] %u bytes, on-air %lu ms (expected ~75 ms): %s\n",
                (unsigned)pkt.length(), (unsigned long)airMs, pkt.c_str());
  if (airMs < 40) {
    Serial.println(F("[TX] WARNING: finished far too fast — modem is not really transmitting"));
  }
  Serial.println();
}
