#include "health.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include "meter.h"

// ---- Einstellungen der Ueberwachung ----
static const uint32_t SCHEDULED_REBOOT_MS = 259200000UL;   // alle 3 Tage, 0 = aus
static const uint32_t WIFI_LOST_MS        = 5UL * 60 * 1000;    // WLAN 5 min weg
static const uint32_t METER_SILENT_MS     = 30UL * 60 * 1000;   // Zaehler 30 min stumm
static const uint32_t MIN_FREE_HEAP       = 16 * 1024;          // weniger als 16 KB frei

static String lastRestart = "unknown";
static uint32_t wifiLostSince = 0;

void healthSetReason(const char *reason) {
  Preferences p;
  p.begin("gwstate", false);
  p.putString("reason", reason);
  p.end();
}

void healthRestart(const char *reason) {
  Serial.printf("[health] restart: %s\n", reason);
  healthSetReason(reason);
  delay(500);
  ESP.restart();
}

void healthBegin() {
  Preferences p;
  p.begin("gwstate", false);
  String stored = p.getString("reason", "");
  p.remove("reason");
  p.end();

  esp_reset_reason_t rr = esp_reset_reason();
  switch (rr) {
    case ESP_RST_POWERON:  lastRestart = "Power on"; break;
    case ESP_RST_EXT:      lastRestart = "External reset"; break;
    case ESP_RST_SW:       lastRestart = stored.length() ? stored : String("Software restart"); break;
    case ESP_RST_PANIC:    lastRestart = "Crash (panic)"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      lastRestart = "Watchdog (system hang)"; break;
    case ESP_RST_BROWNOUT: lastRestart = "Brownout (supply voltage dip)"; break;
    default:               lastRestart = "Other (" + String((int)rr) + ")"; break;
  }
}

String healthLastRestart() {
  return lastRestart;
}

void healthLoop() {
  uint32_t now = millis();

  if (SCHEDULED_REBOOT_MS && now >= SCHEDULED_REBOOT_MS) {
    healthRestart("Scheduled (every 3 days)");
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiLostSince) wifiLostSince = now ? now : 1;
    else if (now - wifiLostSince > WIFI_LOST_MS) healthRestart("WiFi lost for 5 min");
  } else {
    wifiLostSince = 0;
  }

  uint32_t lastOk = meterLastOkMs();   // 0 = seit dem Start nie geantwortet
  if (now > METER_SILENT_MS && now - lastOk > METER_SILENT_MS) {
    healthRestart("Meter silent for 30 min");
  }

  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    healthRestart("Low memory");
  }
}
