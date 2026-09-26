#pragma once
#include <Arduino.h>
#include <ModbusClientRTU.h>

// Momentaufnahme der wichtigsten SDM630-Messwerte
struct MeterValues {
  bool     valid;         // mindestens einmal erfolgreich gelesen
  uint32_t updatedMs;     // millis() der letzten erfolgreichen Abfrage
  float    voltage[3];    // V je Phase
  float    current[3];    // A je Phase
  float    power[3];      // W je Phase (negativ = Einspeisung)
  float    totalPower;    // W gesamt
  float    frequency;     // Hz
  float    importKwh;     // bezogene Energie
  float    exportKwh;     // eingespeiste Energie
  float    totalKwh;      // Gesamtenergie
};

// Startet die zyklische Abfrage im Hintergrund
void meterBegin(ModbusClientRTU *client, uint8_t serverId);
MeterValues meterGet();
uint32_t meterLastOkMs();   // 0 = noch nie erfolgreich
