#pragma once
#include <Arduino.h>

// CPU-Auslastung je Kern, gemessen ueber die Leerlaufzeit (Idle-Task)
void  cpuLoadBegin();              // einmal in setup() aufrufen
void  cpuLoadUpdate();             // regelmaessig in loop() aufrufen, misst jede Sekunde
float cpuLoadNow(uint8_t core);    // Auslastung der letzten Sekunde in %
float cpuLoadAvg(uint8_t core);    // gleitender Mittelwert ueber ca. 1 Minute in %
float cpuLoadPeak(uint8_t core);   // hoechster Sekundenwert seit Start bzw. Reset in %
void  cpuLoadResetPeak();
uint8_t cpuCores();
