// Feather RP2040 Adalogger, four MCP9601s, K-type probes.
// Arduino-Pico 6.2.0; select 8MB flash / 2MB filesystem.
// Samples are appended to LittleFS, committed every 250ms, then copied
// to SD every 3 seconds. The journal is removed only after SD readback.
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SdFat.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>

constexpr uint8_t ADDRESSES[] = {0x64, 0x65, 0x66, 0x67};
constexpr uint32_t SAMPLE_MS = 20;
constexpr uint32_t DUMP_MS = 3000;
constexpr uint32_t FLASH_SYNC_MS = 250;
constexpr char JOURNAL[] = "/pending.bin";
constexpr char CSV_HEADER[] = "Time_ms,TC_0x64_C,TC_0x65_C,TC_0x66_C,TC_0x67_C,Status_0x64,Status_0x65,Status_0x66,Status_0x67\n";
constexpr uint32_t MAGIC = 0x41444832;
struct JournalHeader {
  uint32_t magic;
  char filename[20];
  uint32_t offset;
};

SdFat sd;
SdSpiConfig sdConfig(23, DEDICATED_SPI, SD_SCK_MHZ(8), &SPI1);
Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
File stage;
char filename[20];
uint32_t sdOffset = 0, lastSample = 0, batchStart = 0, lastSync = 0;
uint32_t lastSerial = 0, rows = 0, batches = 0, missedIntervals = 0;
uint32_t batchBytes = 0, batchHash = 2166136261u;
bool paused = false;
float temperatures[4] = {NAN, NAN, NAN, NAN};
const char *states[4] = {"I2C_ERROR", "I2C_ERROR", "I2C_ERROR", "I2C_ERROR"};

void setPixel(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

void fatal(const char *message) {
  // Never discard pending flash data on a storage failure.
  if (stage) { stage.flush(); stage.close(); }
  while (true) {
    if (Serial) { Serial.print("LOGGER ERROR: "); Serial.println(message); }
    setPixel(50, 0, 0); delay(250);
    setPixel(0, 0, 0); delay(750);
  }
}

uint32_t hashBytes(uint32_t hash, const uint8_t *data, size_t count) {
  while (count--) { hash = (hash ^ *data++) * 16777619u; }
  return hash;
}

bool readRegister(uint8_t address, uint8_t reg, uint8_t *data, uint8_t count) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(address, count) != count) return false;
  for (uint8_t i = 0; i < count; ++i) data[i] = Wire.read();
  return true;
}

bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg); Wire.write(value);
  return Wire.endTransmission() == 0;
}

void initSensors() {
  Wire.setSDA(2); Wire.setSCL(3); Wire.begin(); Wire.setClock(100000);
  Wire.setTimeout(20);
  for (uint8_t address : ADDRESSES) {
    uint8_t id[2];
    bool ok = readRegister(address, 0x20, id, 2) && id[0] == 0x41;
    // K type, no digital filter; 18-bit ADC, continuous conversion.
    ok = ok && writeRegister(address, 0x05, 0x00) && writeRegister(address, 0x06, 0x00);
    Serial.printf("MCP9601 0x%02X: %s\n", address, ok ? "connected" : "not responding");
  }
  delay(400);
}

void printTemperatures() {
  if (!Serial || Serial.availableForWrite() < 240) return;
  for (uint8_t i = 0; i < 4; ++i) {
    if (i) Serial.print(" | ");
    Serial.printf("0x%02X: ", ADDRESSES[i]);
    if (!strcmp(states[i], "OK")) {
      Serial.printf("%.2f C / %.2f F", temperatures[i], temperatures[i] * 1.8f + 32);
    } else if (!strcmp(states[i], "OPEN")) Serial.print("OPEN PROBE (no thermocouple)");
    else if (!strcmp(states[i], "SHORT")) Serial.print("SHORT CIRCUIT (check wiring)");
    else Serial.print("ERROR I2C communication");
  }
  Serial.println();
}

void sample() {
  uint32_t timestamp = millis();
  bool fault = false;
  char values[4][20];
  for (uint8_t i = 0; i < 4; ++i) {
    uint8_t status, raw[2];
    temperatures[i] = NAN;
    if (!readRegister(ADDRESSES[i], 0x04, &status, 1) ||
        !readRegister(ADDRESSES[i], 0x00, raw, 2)) states[i] = "I2C_ERROR";
    else if (status & 0x10) states[i] = "OPEN";
    else if (status & 0x20) states[i] = "SHORT";
    else {
      states[i] = "OK";
      temperatures[i] = int16_t((uint16_t(raw[0]) << 8) | raw[1]) / 16.0f;
    }
    fault |= strcmp(states[i], "OK") != 0;
    if (isnan(temperatures[i])) values[i][0] = 0; // Missing value, never a fake zero.
    else snprintf(values[i], sizeof(values[i]), "%.4f", temperatures[i]);
  }
  char row[192];
  int length = snprintf(row, sizeof(row), "%lu,%s,%s,%s,%s,%s,%s,%s,%s\n",
      (unsigned long)timestamp, values[0], values[1], values[2], values[3],
      states[0], states[1], states[2], states[3]);
  if (length <= 0 || size_t(length) >= sizeof(row)) fatal("CSV row overflow");
  if (stage.write((const uint8_t *)row, length) != size_t(length)) fatal("Flash write failed");
  batchBytes += length;
  batchHash = hashBytes(batchHash, (const uint8_t *)row, length);
  ++rows;
  if (timestamp < 1000) setPixel(fault ? 25 : 0, fault ? 10 : 25, 0);
  else setPixel(0, 0, 0);
}

void startBatch() {
  JournalHeader header = {};
  header.magic = MAGIC;
  strncpy(header.filename, filename, sizeof(header.filename) - 1);
  header.offset = sdOffset;
  stage = LittleFS.open(JOURNAL, "w");
  if (!stage || stage.write((const uint8_t *)&header, sizeof(header)) != sizeof(header))
    fatal("Cannot create flash journal");
  stage.flush();
  batchBytes = 0; batchHash = 2166136261u;
  batchStart = lastSync = millis();
}

void transferJournal(bool recovery) {
  if (!LittleFS.exists(JOURNAL)) return;
  File input = LittleFS.open(JOURNAL, "r");
  JournalHeader header = {};
  if (!input || input.read((uint8_t *)&header, sizeof(header)) != sizeof(header) ||
      header.magic != MAGIC || header.filename[19] != 0 ||
      strncmp(header.filename, "FLIGHT", 6) != 0) fatal("Invalid flash journal; preserved");
  // Only replay complete rows after an interrupted write.
  uint32_t bytes = 0, completeBytes = 0, hash = 2166136261u, completeHash = hash;
  while (input.available()) {
    int value = input.read();
    if (value < 0) fatal("Flash read failed");
    uint8_t c = value;
    hash = hashBytes(hash, &c, 1); ++bytes;
    if (c == '\n') { completeBytes = bytes; completeHash = hash; }
  }
  if (!recovery && (bytes != batchBytes || bytes != completeBytes || hash != batchHash))
    fatal("Flash verification failed");
  FsFile output = sd.open(header.filename, O_RDWR); // Never silently replace a missing SD file.
  if (!output || output.fileSize() < header.offset || header.offset < strlen(CSV_HEADER))
    fatal("Journal destination missing or too short; use original SD card");
  if (!output.seekSet(header.offset) || !input.seek(sizeof(header))) fatal("Seek failed");
  uint8_t buffer[512];
  uint32_t remaining = completeBytes;
  while (remaining) {
    size_t count = min(remaining, uint32_t(sizeof(buffer)));
    if (input.read(buffer, count) != int(count) || output.write(buffer, count) != count)
      fatal("SD copy failed; flash journal preserved");
    remaining -= count;
  }
  // Fixed destination offset makes retry safe after an interrupted SD copy.
  uint32_t endOffset = header.offset + completeBytes;
  if (!output.truncate(endOffset) || !output.sync()) fatal("SD sync failed; journal preserved");
  output.close();
  output = sd.open(header.filename, O_RDONLY);
  if (!output || output.fileSize() != endOffset || !output.seekSet(header.offset))
    fatal("SD reopen failed");
  uint32_t verifyHash = 2166136261u;
  remaining = completeBytes;
  while (remaining) {
    size_t count = min(remaining, uint32_t(sizeof(buffer)));
    if (output.read(buffer, count) != int(count)) fatal("SD readback failed");
    verifyHash = hashBytes(verifyHash, buffer, count); remaining -= count;
  }
  if (verifyHash != completeHash) fatal("SD verification failed; journal preserved");
  output.close(); input.close();
  if (!LittleFS.remove(JOURNAL)) fatal("Cannot retire flash journal");
  if (!recovery) { sdOffset = endOffset; ++batches; }
  if (Serial) Serial.printf("%s %s: %lu bytes, SD readback verified\n",
      recovery ? "RECOVERED" : "SD DUMP", header.filename, (unsigned long)completeBytes);
}

void finishBatch() {
  uint32_t started = millis();
  stage.flush(); stage.close();
  transferJournal(false);
  if (Serial) Serial.printf("Dump duration: %lu ms\n", (unsigned long)(millis() - started));
}

void createFlight() {
  bool created = false;
  for (uint32_t i = 0; i < 10000; ++i) {
    snprintf(filename, sizeof(filename), "FLIGHT%04lu.CSV", (unsigned long)i);
    if (sd.exists(filename)) continue;
    FsFile output = sd.open(filename, O_WRONLY | O_CREAT | O_EXCL);
    if (!output || output.write(CSV_HEADER, strlen(CSV_HEADER)) != strlen(CSV_HEADER) || !output.sync())
      fatal("Cannot create SD log file");
    sdOffset = output.fileSize(); output.close(); created = true; break;
  }
  if (!created) fatal("FLIGHT0000 through FLIGHT9999 already exist");
  Serial.printf("LOG FILE: %s\n", filename);
}

void inspectLog() {
  Serial.printf("STATUS file=%s rows=%lu batches=%lu skipped_20ms_slots=%lu paused=%d\n",
      filename, (unsigned long)rows, (unsigned long)batches, (unsigned long)missedIntervals, paused);
  FsFile input = sd.open(filename, O_RDONLY);
  if (!input) { Serial.println("Cannot inspect SD log"); return; }
  Serial.printf("SD FILE SIZE: %lu\n", (unsigned long)input.fileSize());
  Serial.println("SD PREVIEW BEGIN");
  uint8_t lines = 0;
  while (input.available() && lines < 5) {
    int c = input.read(); Serial.write(c); if (c == '\n') ++lines;
  }
  input.close(); Serial.println("SD PREVIEW END");
}

void setup() {
  Serial.begin(115200);
  pixel.begin(); setPixel(25, 25, 25);
  uint32_t waitStart = millis();
  while (!Serial && millis() - waitStart < 1500) delay(10);
  Serial.println("Thermocouple Data Logger MCP9601: flash -> SD every 3 seconds");
  LittleFSConfig fsConfig; fsConfig.setAutoFormat(false); LittleFS.setConfig(fsConfig);
  if (!LittleFS.begin()) {
    // Explicit one-time provisioning. A later mount failure never erases data automatically.
    Serial.setTimeout(1000);
    while (true) {
      Serial.println("FLASH_NOT_INITIALIZED: send FORMAT followed by newline to initialize the 2MB flash partition");
      uint32_t start = millis();
      while (millis() - start < 2000) {
        if (Serial.available()) {
          String command = Serial.readStringUntil('\n'); command.trim();
          if (command == "FORMAT") {
            if (!LittleFS.format() || !LittleFS.begin()) fatal("Flash format failed");
            goto flashReady;
          }
        }
        delay(10);
      }
    }
  }
flashReady:
  SPI1.setRX(20); SPI1.setTX(19); SPI1.setSCK(18);
  while (!sd.begin(sdConfig)) {
    // Some cards retain a valid filesystem at the 1 MiB boundary but have
    // no MBR partition entry pointing to it. Let SdFat validate that volume.
    if (sd.card() && !sd.sdErrorCode() && sd.card()->sectorCount() > 2048 &&
        sd.vol()->begin(sd.card(), true, 0, 2048)) break;
    if (Serial) {
      Serial.println("WAITING FOR SD: insert a FAT32/exFAT microSD card");
      sd.initErrorPrint(&Serial);
      if (sd.card() && !sd.sdErrorCode()) {
        Serial.printf("SD capacity: %lu sectors\n", (unsigned long)sd.card()->sectorCount());
        uint8_t sector[512];
        if (sd.card()->readSector(0, sector)) {
          Serial.printf("SD MBR signature=%02X%02X; partition types=%02X,%02X,%02X,%02X\n",
              sector[511], sector[510], sector[450], sector[466], sector[482], sector[498]);
          Serial.print("SD boot identifier: ");
          for (uint8_t i = 3; i < 11; ++i) Serial.write(sector[i] >= 32 && sector[i] <= 126 ? sector[i] : '.');
          Serial.println();
          Serial.print("Sector 0 first 32 bytes: ");
          for (uint8_t i = 0; i < 32; ++i) Serial.printf("%02X ", sector[i]);
          Serial.println();
          if (sd.card()->readSector(2048, sector))
            Serial.printf("Sector 2048 boot signature=%02X%02X\n", sector[511], sector[510]);
        }
      }
    }
    setPixel(50, 0, 0); delay(250);
    setPixel(0, 0, 0); delay(1750);
  }
  Serial.println("SD initialized");
  transferJournal(true);
  initSensors(); createFlight(); startBatch();
  lastSample = millis();
  Serial.println("Commands: s=flush and stop, r=resume, i=inspect SD file");
}

void loop() {
  if (Serial.available()) {
    char command = Serial.read();
    if (command == 's' && !paused) {
      finishBatch(); paused = true; Serial.println("STOPPED: SD synced; safe to power off");
    } else if (command == 'r' && paused) {
      startBatch(); lastSample = millis(); paused = false; Serial.println("RESUMED");
    } else if (command == 'i') inspectLog();
  }
  if (paused) { delay(1); return; }
  uint32_t now = millis();
  if (uint32_t(now - lastSample) >= SAMPLE_MS) {
    uint32_t slots = uint32_t(now - lastSample) / SAMPLE_MS;
    missedIntervals += slots - 1;
    lastSample += slots * SAMPLE_MS;
    sample();
  }
  now = millis();
  if (uint32_t(now - batchStart) >= DUMP_MS) { finishBatch(); startBatch(); }
  else if (uint32_t(now - lastSync) >= FLASH_SYNC_MS) { stage.flush(); lastSync = now; }
  if (uint32_t(now - lastSerial) >= 1000) { printTemperatures(); lastSerial = now; }
}
