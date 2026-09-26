#pragma once
#include <Arduino.h>

void healthBegin();                       // beim Start aufrufen: letzten Neustartgrund ermitteln
void healthLoop();                        // in loop() aufrufen: Ueberwachung
void healthSetReason(const char *reason); // Grund vormerken (z. B. vor OTA)
void healthRestart(const char *reason);   // Grund speichern und neu starten
String healthLastRestart();               // Grund des letzten Neustarts
