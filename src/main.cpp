#include <WiFi.h>
#include <AsyncTCP.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Logging.h>
#include <ModbusBridgeWiFi.h>
#include <ModbusClientRTU.h>
#include "config.h"
#include "pages.h"
#include "errorstats.h"
#include "meter.h"
#include "health.h"
#include "cpuload.h"
#include "clients.h"
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
// 13.06.2026 Upload via OTA hinzugefügt
// 26.09.2026 Fehlerstatistik nach Fehlerart hinzugefügt
// 26.09.2026 Fehlerprotokoll mit NTP-Uhrzeit und Antwort-Cache hinzugefügt
// 26.09.2026 Zaehlerwerte, Systemueberwachung und Statistik-Reset hinzugefügt
// 26.09.2026 CPU-Last und Liste der verbundenen Clients hinzugefügt

AsyncWebServer webServer(80);
Config config;
Preferences prefs;
// Macht syncRequestM zugaenglich, das in eModbus nur protected ist
class RTUClient : public ModbusClientRTU {
public:
  using ModbusClientRTU::ModbusClientRTU;   // Konstruktoren uebernehmen
  using ModbusClientRTU::syncRequestM;      // Funktion oeffentlich machen
};
RTUClient *MBclient;
ModbusBridgeWiFi MBbridge;
WiFiManager wm;
// Set your Static IP address
IPAddress local_IP(192, 168, 1, 214);
// Set your Gateway IP address
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
//IPAddress primaryDNS(8, 8, 8, 8);   //optional
//IPAddress secondaryDNS(8, 8, 4, 4); //optional
String ipr;
const uint8_t METER_ID = 1;   // Modbus-Adresse des SDM630

// ---------------------------------------------------------------------------
// Antwort-Cache: Fragen mehrere Clients kurz hintereinander dieselben Register
// ab, geht nur die erste Anfrage auf den Bus. Nur Lesebefehle (FC 1-4) werden
// zwischengespeichert, jeder Schreibbefehl leert den Cache.
// ---------------------------------------------------------------------------
const uint32_t CACHE_MS = 500;     // so lange gilt eine Antwort als aktuell
const uint8_t  CACHE_SLOTS = 16;   // Anzahl verschiedener Abfragen im Cache

struct CacheEntry {
  uint64_t key;
  uint32_t stamp;
  bool used;
  ModbusMessage response;
};
static CacheEntry cache[CACHE_SLOTS];
static SemaphoreHandle_t busMutex;

// Leitet eine Modbus-TCP-Anfrage an den RTU-Bus weiter (oder beantwortet sie aus dem Cache)
ModbusMessage forwardToRtu(ModbusMessage request) {
  uint8_t sid = request.getServerID();
  uint8_t fc = request.getFunctionCode();
  uint16_t reg = 0, cnt = 0;
  if (request.size() >= 6) {
    reg = (request[2] << 8) | request[3];
    cnt = (request[4] << 8) | request[5];
  }
  bool cacheable = (fc >= 1 && fc <= 4) && request.size() == 6;
  uint64_t key = ((uint64_t)sid << 40) | ((uint64_t)fc << 32) | ((uint32_t)reg << 16) | cnt;

  // Nur eine Busanfrage gleichzeitig. Wartende Clients pruefen danach zuerst den Cache,
  // so werden auch gleichzeitig eintreffende identische Anfragen zusammengefasst.
  xSemaphoreTake(busMutex, portMAX_DELAY);

  if (cacheable) {
    uint32_t now = millis();
    for (auto &c : cache) {
      if (c.used && c.key == key && now - c.stamp < CACHE_MS) {
        ModbusMessage hit = c.response;
        xSemaphoreGive(busMutex);
        errorStatsCacheHit();
        return hit;
      }
    }
  } else {
    for (auto &c : cache) c.used = false;   // Schreibzugriff: alte Werte verwerfen
  }

  ModbusMessage response = MBclient->syncRequestM(request, (uint32_t)millis());
  Modbus::Error err = response.getError();

  if (cacheable && err == SUCCESS) {
    uint32_t now = millis();
    CacheEntry *slot = nullptr;
    for (auto &c : cache) if (c.used && c.key == key) { slot = &c; break; }
    if (!slot) for (auto &c : cache) if (!c.used) { slot = &c; break; }
    if (!slot) {                             // alles belegt: aeltesten Eintrag ersetzen
      slot = &cache[0];
      for (auto &c : cache) if (now - c.stamp > now - slot->stamp) slot = &c;
    }
    slot->key = key;
    slot->stamp = now;
    slot->used = true;
    slot->response = response;
  }
  xSemaphoreGive(busMutex);

  errorStatsRecord(sid, fc, reg, cnt, err);
  // Interne Fehler (Timeout, CRC, ...) sind keine gueltigen Modbus-Codes.
  // Der TCP-Client bekommt dafuer die Standardmeldung "Gateway target no response".
  if (static_cast<uint8_t>(err) > 0x0B) {
    response.setError(sid, fc, GATEWAY_TARGET_NO_RESP);
  }
  return response;
}

void setup() {
  debugSerial.begin(115200);
  dbgln();
  dbgln("[config] load")
  prefs.begin("modbusRtuGw");
  config.begin(&prefs);
  healthBegin();   // Grund des letzten Neustarts ermitteln
  cpuLoadBegin();  // CPU-Auslastung messen
  debugSerial.end();
  debugSerial.begin(config.getSerialBaudRate(), config.getSerialConfig());
  dbgln("[wifi] start");

  // Configures static IP address
  ipr=config.getipAdr();
  local_IP.fromString(ipr);
  if (!WiFi.config(local_IP, gateway, subnet, gateway)) {   // Router auch als DNS (fuer NTP)
    Serial.println("STA Failed to configure");
  }
  wm.setClass("invert");
  auto reboot = false;
  wm.setAPCallback([&reboot](WiFiManager *wifiManager){reboot = true;});
  wm.setConnectTimeout(30);        // 30 s Verbindungsversuch
  wm.setConfigPortalTimeout(180);  // Portal nach 3 min schließen
  if (!wm.autoConnect()) {
    ESP.restart();                 // WLAN nicht gefunden -> neu versuchen
  }
  if (reboot) {
    ESP.restart();                 // nach Portal-Konfiguration sauber neu starten
  }
  dbgln("[wifi] finished");
  // Uhrzeit per NTP (deutsche Zeitzone inkl. Sommerzeit) fuer das Fehlerprotokoll
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "de.pool.ntp.org", "pool.ntp.org");
  dbgln("[modbus] start");
  ArduinoOTA.setHostname("modbus2eth");
  ArduinoOTA.onStart([](){ healthSetReason("OTA update"); });
  ArduinoOTA.begin();

  MBUlogLvl = LOG_LEVEL_WARNING;
  RTUutils::prepareHardwareSerial(modbusSerial);
#if defined(RX_PIN) && defined(TX_PIN)
  // use rx and tx-pins if defined in platformio.ini
  modbusSerial.begin(config.getModbusBaudRate(), config.getModbusConfig(), RX_PIN, TX_PIN );
  dbgln("Use user defined RX/TX pins");
#else
  // otherwise use default pins for hardware-serial2
  modbusSerial.begin(config.getModbusBaudRate(), config.getModbusConfig(), config.getrxpin(), config.gettxpin());
#endif

  MBclient = new RTUClient(config.getModbusRtsPin());
  MBclient->setTimeout(1000);
  MBclient->begin(modbusSerial, 1);
  busMutex = xSemaphoreCreateMutex();
  // Statt attachServer: eigene Weiterleitung, damit jede Fehlerart gezaehlt wird
  for (uint8_t i = 1; i < 248; i++)
  {
    MBbridge.registerWorker(i, ANY_FUNCTION_CODE, &forwardToRtu);
  }
  MBbridge.start(config.getTcpPort(), 10, config.getTcpTimeout());
  meterBegin(MBclient, METER_ID);   // Zaehlerwerte im Hintergrund abfragen
  clientsBegin(config.getTcpPort()); // verbundene Modbus-Clients erfassen
  dbgln("[modbus] finished");
  setupPages(&webServer, MBclient, &MBbridge, &config, &wm);
  webServer.begin();
  dbgln("[setup] finished");
}

void loop() {
  ArduinoOTA.handle();
  healthLoop();   // geplanter Neustart, WLAN-, Zaehler- und Speicherueberwachung
  cpuLoadUpdate();
  clientsUpdate();
  delay(10);
}
