#pragma once
#include <Arduino.h>
#include <time.h>
#include <ModbusTypeDefs.h>

#define ERROR_LOG_SIZE 20

// Ein Eintrag im Fehlerprotokoll
struct ErrorLogEntry {
  time_t   epoch;     // Uhrzeit per NTP, 0 = Uhr war noch nicht synchron
  uint32_t ms;        // millis() zum Zeitpunkt des Fehlers
  uint8_t  serverId;  // Modbus-Slave-ID
  uint8_t  fc;        // Funktionscode
  uint8_t  code;      // Fehlerart (Modbus::Error)
  uint16_t reg;       // Startregister
  uint16_t count;     // Anzahl Register
};

// Zaehlt eine echte Busanfrage und protokolliert sie im Fehlerfall
void errorStatsRecord(uint8_t serverId, uint8_t fc, uint16_t reg, uint16_t count, Modbus::Error e);
// Zaehlt eine Anfrage, die aus dem Cache beantwortet wurde
void errorStatsCacheHit();

uint32_t errorStatsTotal();           // Busanfragen gesamt
uint32_t errorStatsFailed();          // davon fehlgeschlagen
uint32_t errorStatsGet(uint8_t code); // Anzahl fuer eine bestimmte Fehlerart
uint32_t errorStatsLastMs();          // millis() des letzten Fehlers (0 = noch keiner)
uint8_t  errorStatsLastCode();        // Fehlerart des letzten Fehlers
uint32_t errorStatsCacheHits();       // aus dem Cache beantwortete Anfragen

// Setzt alle Zaehler und das Fehlerprotokoll zurueck
void errorStatsReset();

// Kopiert das Fehlerprotokoll nach out, neuester Eintrag zuerst. Rueckgabe: Anzahl Eintraege
uint8_t errorStatsLog(ErrorLogEntry *out, uint8_t max);
