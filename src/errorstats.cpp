#include "errorstats.h"

// Ein Zaehler je moeglichem Fehlercode (1 KB RAM)
static uint32_t counts[256];
static uint32_t total = 0;
static uint32_t failed = 0;
static uint32_t cacheHits = 0;
static uint32_t lastErrorMs = 0;
static uint8_t  lastErrorCode = 0;

// Ringpuffer fuer die letzten Fehler
static ErrorLogEntry logBuf[ERROR_LOG_SIZE];
static uint8_t logHead = 0;   // naechster Schreibplatz
static uint8_t logFill = 0;   // belegte Eintraege

// Jeder TCP-Client laeuft in einem eigenen Task, daher gegen gleichzeitigen Zugriff absichern
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void errorStatsRecord(uint8_t serverId, uint8_t fc, uint16_t reg, uint16_t count, Modbus::Error e){
  uint32_t now = millis();
  uint8_t code = static_cast<uint8_t>(e);
  time_t t = 0;
  if (code != 0) {
    t = time(nullptr);
    if (t < 1700000000) t = 0;   // Uhr noch nicht per NTP gestellt
  }
  portENTER_CRITICAL(&mux);
  total++;
  if (code != 0) {
    failed++;
    counts[code]++;
    lastErrorMs = now ? now : 1;   // 0 ist fuer "noch kein Fehler" reserviert
    lastErrorCode = code;
    ErrorLogEntry &en = logBuf[logHead];
    en.epoch = t;
    en.ms = now;
    en.serverId = serverId;
    en.fc = fc;
    en.code = code;
    en.reg = reg;
    en.count = count;
    logHead = (logHead + 1) % ERROR_LOG_SIZE;
    if (logFill < ERROR_LOG_SIZE) logFill++;
  }
  portEXIT_CRITICAL(&mux);
}

void errorStatsCacheHit(){
  portENTER_CRITICAL(&mux);
  cacheHits++;
  portEXIT_CRITICAL(&mux);
}

uint32_t errorStatsTotal()            { return total; }
uint32_t errorStatsFailed()           { return failed; }
uint32_t errorStatsGet(uint8_t code)  { return counts[code]; }
uint32_t errorStatsLastMs()           { return lastErrorMs; }
uint8_t  errorStatsLastCode()         { return lastErrorCode; }
uint32_t errorStatsCacheHits()        { return cacheHits; }

uint8_t errorStatsLog(ErrorLogEntry *out, uint8_t max){
  portENTER_CRITICAL(&mux);
  uint8_t n = logFill < max ? logFill : max;
  for (uint8_t i = 0; i < n; i++) {
    // rueckwaerts ab dem zuletzt geschriebenen Eintrag
    uint8_t idx = (logHead + ERROR_LOG_SIZE - 1 - i) % ERROR_LOG_SIZE;
    out[i] = logBuf[idx];
  }
  portEXIT_CRITICAL(&mux);
  return n;
}

void errorStatsReset(){
  portENTER_CRITICAL(&mux);
  memset(counts, 0, sizeof(counts));
  total = 0;
  failed = 0;
  cacheHits = 0;
  lastErrorMs = 0;
  lastErrorCode = 0;
  logHead = 0;
  logFill = 0;
  portEXIT_CRITICAL(&mux);
}
