#pragma once
#include <Arduino.h>
#include <time.h>

// Merkt sich, welche IP-Adressen mit dem Modbus-TCP-Port verbunden sind oder waren
struct ClientInfo {
  uint32_t ip;          // IPv4-Adresse (lwIP-Format, mit IPAddress(ip) umwandelbar)
  uint8_t  active;      // aktuell offene Verbindungen von dieser IP
  uint16_t connects;    // Anzahl neu aufgebauter Verbindungen seit dem Start
  uint32_t firstMs;     // millis() beim ersten Auftauchen
  uint32_t lastMs;      // millis() beim letzten Auftauchen
  time_t   firstEpoch;  // Uhrzeit beim ersten Auftauchen (0 = Uhr noch nicht gestellt)
  time_t   lastEpoch;   // Uhrzeit beim letzten Auftauchen
};

void clientsBegin(uint16_t port);                 // Port, auf dem die Bridge lauscht
void clientsUpdate();                             // in loop() aufrufen, prueft alle paar Sekunden
uint8_t clientsGet(ClientInfo *out, uint8_t max); // verbundene zuerst, dann zuletzt gesehene
