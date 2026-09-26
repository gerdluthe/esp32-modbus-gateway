#include "clients.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/priv/tcp_priv.h"

// Wie oft die offenen Verbindungen geprueft werden. Die Abfrage dauert nur Mikrosekunden,
// ein kurzer Abstand faengt aber auch Clients ein, die nur kurz verbunden sind.
static const uint32_t SCAN_MS = 5000;

#define MAX_CONN    16   // gleichzeitig erfasste Verbindungen
#define MAX_CLIENTS 16   // gemerkte IP-Adressen
#define MAX_PORTS    4   // gemerkte Verbindungen je IP

struct ConnSnap { uint32_t ip; uint16_t port; };

// Aufruf-Struktur fuer den lwIP-Thread (erstes Element muss tcpip_api_call_data sein)
struct SnapCall {
  struct tcpip_api_call_data call;
  uint16_t localPort;
  uint8_t  n;
  ConnSnap conn[MAX_CONN];
};

struct Tracked {
  bool     used;
  ClientInfo info;
  uint8_t  nPorts;
  uint16_t ports[MAX_PORTS];
};

static uint16_t listenPort = 502;
static uint32_t lastScan = 0;
static Tracked tbl[MAX_CLIENTS];
static SnapCall snap;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Laeuft im Netzwerk-Thread: nur dort darf die Verbindungsliste gelesen werden
static err_t snapInTcpip(struct tcpip_api_call_data *c) {
  SnapCall *s = (SnapCall *)c;
  s->n = 0;
  for (struct tcp_pcb *p = tcp_active_pcbs; p != nullptr && s->n < MAX_CONN; p = p->next) {
    if (p->local_port != s->localPort || p->state != ESTABLISHED) continue;
    if (!IP_IS_V4(&p->remote_ip)) continue;
    s->conn[s->n].ip = ip_2_ip4(&p->remote_ip)->addr;
    s->conn[s->n].port = p->remote_port;
    s->n++;
  }
  return ERR_OK;
}

void clientsBegin(uint16_t port) {
  listenPort = port;
  lastScan = millis() - SCAN_MS;   // gleich beim ersten Durchlauf pruefen
}

void clientsUpdate() {
  uint32_t now = millis();
  if (now - lastScan < SCAN_MS) return;
  lastScan = now;

  snap.localPort = listenPort;
  tcpip_api_call(snapInTcpip, &snap.call);

  time_t t = time(nullptr);
  if (t < 1700000000) t = 0;

  uint8_t  newN[MAX_CLIENTS] = {0};
  uint16_t newPorts[MAX_CLIENTS][MAX_PORTS] = {{0}};

  portENTER_CRITICAL(&mux);
  for (uint8_t i = 0; i < snap.n; i++) {
    uint32_t ip = snap.conn[i].ip;
    uint16_t port = snap.conn[i].port;

    // bekannte IP suchen, sonst freien Platz, sonst den am laengsten nicht gesehenen ersetzen
    int idx = -1;
    for (int k = 0; k < MAX_CLIENTS; k++) if (tbl[k].used && tbl[k].info.ip == ip) { idx = k; break; }
    if (idx < 0) for (int k = 0; k < MAX_CLIENTS; k++) if (!tbl[k].used) { idx = k; break; }
    if (idx < 0) {
      idx = 0;
      for (int k = 1; k < MAX_CLIENTS; k++)
        if (now - tbl[k].info.lastMs > now - tbl[idx].info.lastMs) idx = k;
      tbl[idx].used = false;
    }
    Tracked &tr = tbl[idx];
    if (!tr.used) {
      tr = Tracked();
      tr.used = true;
      tr.info.ip = ip;
      tr.info.firstMs = now;
      tr.info.firstEpoch = t;
    }
    // Port beim letzten Durchlauf noch nicht offen = neue Verbindung
    bool known = false;
    for (uint8_t k = 0; k < tr.nPorts; k++) if (tr.ports[k] == port) known = true;
    if (!known) tr.info.connects++;
    if (newN[idx] < MAX_PORTS) newPorts[idx][newN[idx]++] = port;
  }
  for (int k = 0; k < MAX_CLIENTS; k++) {
    if (!tbl[k].used) continue;
    tbl[k].nPorts = newN[k];
    for (uint8_t p = 0; p < newN[k]; p++) tbl[k].ports[p] = newPorts[k][p];
    tbl[k].info.active = newN[k];
    if (newN[k]) {
      tbl[k].info.lastMs = now;
      tbl[k].info.lastEpoch = t;
    }
  }
  portEXIT_CRITICAL(&mux);
}

uint8_t clientsGet(ClientInfo *out, uint8_t max) {
  uint8_t n = 0;
  portENTER_CRITICAL(&mux);
  for (int k = 0; k < MAX_CLIENTS && n < max; k++) if (tbl[k].used) out[n++] = tbl[k].info;
  portEXIT_CRITICAL(&mux);
  // verbundene zuerst, danach nach letztem Auftauchen
  uint32_t now = millis();
  for (uint8_t i = 1; i < n; i++) {
    for (uint8_t j = i; j > 0; j--) {
      bool swap = (out[j].active > 0 && out[j - 1].active == 0) ||
                  ((out[j].active > 0) == (out[j - 1].active > 0) &&
                   now - out[j].lastMs < now - out[j - 1].lastMs);
      if (!swap) break;
      ClientInfo tmp = out[j]; out[j] = out[j - 1]; out[j - 1] = tmp;
    }
  }
  return n;
}
