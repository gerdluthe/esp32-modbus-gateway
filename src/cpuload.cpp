#include "cpuload.h"
#include <esp_freertos_hooks.h>

// Prinzip: Der Leerlauf-Task jedes Kerns zaehlt mit, wie oft er zum Zug kommt.
// Der hoechste je gemessene Wert pro Sekunde entspricht 0 % Auslastung,
// jede Sekunde mit weniger Leerlauf-Durchlaeufen bedeutet entsprechend mehr Last.

static const uint8_t CORES = portNUM_PROCESSORS;
static volatile uint32_t idleCnt[2] = {0, 0};
static uint32_t lastCnt[2]  = {0, 0};
static uint32_t idleRef[2]  = {1, 1};   // Leerlauf-Durchlaeufe pro Sekunde bei 0 % Last
static float loadNow[2]  = {0, 0};
static float loadAvg[2]  = {0, 0};
static float loadPeak[2] = {0, 0};
static uint32_t lastSample = 0;
static bool firstSample = true;

// false = sofort erneut aufrufen, so misst der Zaehler die volle Leerlaufzeit
static bool IRAM_ATTR idleHook0() { idleCnt[0]++; return false; }
static bool IRAM_ATTR idleHook1() { idleCnt[1]++; return false; }

void cpuLoadBegin() {
  esp_register_freertos_idle_hook_for_cpu(idleHook0, 0);
  if (CORES > 1) esp_register_freertos_idle_hook_for_cpu(idleHook1, 1);
  lastSample = millis();
}

void cpuLoadUpdate() {
  uint32_t now = millis();
  uint32_t dt = now - lastSample;
  if (dt < 1000) return;
  lastSample = now;

  for (uint8_t c = 0; c < CORES; c++) {
    uint32_t cnt = idleCnt[c];
    uint32_t perSec = (uint32_t)((uint64_t)(cnt - lastCnt[c]) * 1000 / dt);
    lastCnt[c] = cnt;
    if (firstSample) continue;                 // erste Messung ist unvollstaendig
    if (perSec > idleRef[c]) idleRef[c] = perSec;
    float load = 100.0f - 100.0f * perSec / idleRef[c];
    if (load < 0) load = 0;
    loadNow[c] = load;
    loadAvg[c] += (load - loadAvg[c]) / 60.0f; // ca. 1 Minute Glaettung
    if (now > 30000 && load > loadPeak[c]) loadPeak[c] = load;   // Startphase ignorieren
  }
  firstSample = false;
}

float cpuLoadNow(uint8_t core)  { return core < CORES ? loadNow[core] : 0; }
float cpuLoadAvg(uint8_t core)  { return core < CORES ? loadAvg[core] : 0; }
float cpuLoadPeak(uint8_t core) { return core < CORES ? loadPeak[core] : 0; }
void  cpuLoadResetPeak()        { loadPeak[0] = 0; loadPeak[1] = 0; }
uint8_t cpuCores()              { return CORES; }
