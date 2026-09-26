#include "meter.h"

static const uint32_t POLL_MS = 5000;   // alle 5 s abfragen

static ModbusClientRTU *mc = nullptr;
static uint8_t meterId = 1;
static MeterValues vals = {};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Liest n Float-Werte (je 2 Register, Big Endian) ab Register reg per FC 04
static bool readFloats(uint16_t reg, uint8_t n, float *out) {
  ModbusMessage r = mc->syncRequest((uint32_t)(0x4D000000UL | reg), meterId, (uint8_t)0x04, reg, (uint16_t)(n * 2));
  if (r.getError() != SUCCESS || r.size() < (uint16_t)(3 + n * 4)) return false;
  for (uint8_t i = 0; i < n; i++) {
    uint16_t p = 3 + i * 4;
    uint32_t raw = ((uint32_t)r[p] << 24) | ((uint32_t)r[p + 1] << 16) | ((uint32_t)r[p + 2] << 8) | r[p + 3];
    memcpy(&out[i], &raw, 4);
  }
  return true;
}

static void meterTask(void *) {
  for (;;) {
    float block[9], total[1], freq[1], energy[2], sum[1];
    // 0x0000: U L1-L3, I L1-L3, P L1-L3 | 0x0034: P gesamt | 0x0046: Frequenz
    // 0x0048: Bezug, Einspeisung | 0x0156: Gesamtenergie
    bool ok = readFloats(0x0000, 9, block)
           && readFloats(0x0034, 1, total)
           && readFloats(0x0046, 1, freq)
           && readFloats(0x0048, 2, energy)
           && readFloats(0x0156, 1, sum);
    if (ok) {
      MeterValues v;
      v.valid = true;
      v.updatedMs = millis();
      if (v.updatedMs == 0) v.updatedMs = 1;
      for (int i = 0; i < 3; i++) {
        v.voltage[i] = block[i];
        v.current[i] = block[3 + i];
        v.power[i] = block[6 + i];
      }
      v.totalPower = total[0];
      v.frequency = freq[0];
      v.importKwh = energy[0];
      v.exportKwh = energy[1];
      v.totalKwh = sum[0];
      portENTER_CRITICAL(&mux);
      vals = v;
      portEXIT_CRITICAL(&mux);
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }
}

void meterBegin(ModbusClientRTU *client, uint8_t serverId) {
  mc = client;
  meterId = serverId;
  xTaskCreate(meterTask, "meter", 4096, nullptr, 1, nullptr);
}

MeterValues meterGet() {
  portENTER_CRITICAL(&mux);
  MeterValues v = vals;
  portEXIT_CRITICAL(&mux);
  return v;
}

uint32_t meterLastOkMs() {
  portENTER_CRITICAL(&mux);
  uint32_t t = vals.valid ? vals.updatedMs : 0;
  portEXIT_CRITICAL(&mux);
  return t;
}
