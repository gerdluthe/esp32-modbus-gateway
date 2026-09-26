#include "pages.h"
#include "errorstats.h"
#include "meter.h"
#include "health.h"
#include "cpuload.h"
#include "clients.h"
#define ETAG "\"" __DATE__ "" __TIME__ "\""
#define ADMIN_WEB_PASS  \
        if ((!config->getWebPassword().equals("")) && (!request->authenticate("admin", config->getWebPassword().c_str()))) \
            return request->requestAuthentication();
#define WEB_PASS_PLACEHOLDER "****"
String IP_PLACEHOLDER;

// Prueft, ob ein GPIO fuer RX/TX am ESP32 sinnvoll ist
static bool validPin(long p, bool output){
  if (p < 0 || p > 39) return false;
  if (p >= 6 && p <= 11) return false;   // SPI-Flash
  if (output && p >= 34) return false;   // GPIO 34-39 nur Eingang
  return true;
}

static const char SELECT_SCRIPT[] =
  "<script>"
    "(function(){"
      "var s = document.querySelectorAll('select[data-value]');"
      "for(d of s){"
        "var o = d.querySelector(`option[value='${d.dataset.value}']`);"
        "if(o) o.selected=true;"
    "}})();"
  "</script>";

static void sendHead(AsyncResponseStream *response, const char *title, bool inlineStyle);
static void fmtCount(char *out, size_t len, uint32_t v);

void setupPages(AsyncWebServer *server, ModbusClientRTU *rtu, ModbusBridgeWiFi *bridge, Config *config, WiFiManager *wm){
  server->on("/", HTTP_GET, [config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    dbgln("[webserver] GET /");
    auto *response = request->beginResponseStream("text/html");
    sendResponseHeader(response, "Main");
    sendButton(response, "Status", "status");
    sendButton(response, "Config", "config");
    sendButton(response, "Set PINs / IP", "pins");
    sendButton(response, "Debug", "debug");
    sendButton(response, "Firmware update", "update");
    response->print("<hr>");
    sendButton(response, "WiFi reset", "wifi", "r");
    sendButton(response, "Reboot", "reboot", "r");
    sendResponseTrailer(response);
    request->send(response);
  });

  server->on("/status", HTTP_GET, [rtu, bridge, config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    dbgln("[webserver] GET /status");
    auto *response = request->beginResponseStream("text/html");
    char buf[192];

    // ---- Kopf: Titel links, kleines Rollenzaehlwerk rechts ----
    uint64_t s = esp_timer_get_time() / 1000000;
    char uptime[24];
    snprintf(uptime, sizeof(uptime), "%02lu:%02u:%02u",
      (unsigned long)(s / 3600), (unsigned)((s % 3600) / 60), (unsigned)(s % 60));
    sendHead(response, "Status", false);
    response->printf("<header class=\"top st\"><div><small>Modbus Gateway &middot; %s</small><h1>Status</h1></div>"
      "<div class=\"roll mini\" title=\"Uptime hh:mm:ss\">", WiFi.localIP().toString().c_str());
    for (const char *c = uptime; *c; c++) {
      if (*c == ':') response->print("<span class=\"c\">:</span>");
      else response->printf("<span class=\"d%s\">%c</span>", c[1] ? "" : " r", *c);
    }
    response->print("</div></header>");

    // Zaehlerscheibe dreht mit der aktuellen Leistung, bei Einspeisung rueckwaerts
    MeterValues mv = meterGet();
    char discStyle[96] = "";
    if (mv.valid) {
      float p = fabsf(mv.totalPower);
      if (p < 10.0f) {
        strcpy(discStyle, " style=\"animation-play-state:paused\"");
      } else {
        float d = constrain(4000.0f / p, 0.5f, 20.0f);   // 1 kW = 4 s pro Umlauf
        snprintf(discStyle, sizeof(discStyle), " style=\"animation-duration:%.1fs%s\"",
          d, mv.totalPower < 0 ? ";animation-direction:reverse" : "");
      }
    }
    response->printf("<div class=\"disc\"><i%s></i></div>", discStyle);

    // ---- Reiter (reines CSS, die Auswahl merkt sich die Seite in der Adresszeile) ----
    response->print(
      "<input class=\"tabr\" type=\"radio\" name=\"tab\" id=\"t1\" checked>"
      "<input class=\"tabr\" type=\"radio\" name=\"tab\" id=\"t2\">"
      "<input class=\"tabr\" type=\"radio\" name=\"tab\" id=\"t3\">"
      "<input class=\"tabr\" type=\"radio\" name=\"tab\" id=\"t4\">"
      "<div class=\"tabs\">"
        "<label for=\"t1\">Meter</label>"
        "<label for=\"t2\">Modbus</label>"
        "<label for=\"t3\">Network</label>"
        "<label for=\"t4\">System</label>"
      "</div>");

    // ======== Reiter 1: Zaehler ========
    response->print("<div class=\"pane\" id=\"p1\">");
    if (!mv.valid) {
      response->print("<p class=\"hint\">No data from the meter yet.</p>");
    } else {
      snprintf(buf, sizeof(buf), "<div class=\"big\"><b>%.0f W</b><span>%s</span></div>",
        fabsf(mv.totalPower), mv.totalPower < 0 ? "current export to grid" : "current import from grid");
      response->print(buf);
      response->print("<div class=\"ph hd\"><span></span><span>V</span><span>A</span><span>W</span></div>");
      for (int i = 0; i < 3; i++) {
        snprintf(buf, sizeof(buf), "<div class=\"ph\"><span>L%d</span><span>%.1f</span><span>%.2f</span><span>%.0f</span></div>",
          i + 1, mv.voltage[i], mv.current[i], mv.power[i]);
        response->print(buf);
      }
      response->print("<h3>Energy</h3><table>");
      snprintf(buf, sizeof(buf), "%.2f kWh", mv.importKwh);
      sendTableRow(response, "Import", String(buf));
      snprintf(buf, sizeof(buf), "%.2f kWh", mv.exportKwh);
      sendTableRow(response, "Export", String(buf));
      snprintf(buf, sizeof(buf), "%.2f kWh", mv.totalKwh);
      sendTableRow(response, "Total", String(buf));
      snprintf(buf, sizeof(buf), "%.2f Hz", mv.frequency);
      sendTableRow(response, "Frequency", String(buf));
      snprintf(buf, sizeof(buf), "%lu s ago", (unsigned long)((millis() - mv.updatedMs) / 1000));
      sendTableRow(response, "Updated", String(buf));
      response->print("</table>");
    }
    response->print("</div>");

    // ======== Reiter 2: Modbus ========
    response->print("<div class=\"pane\" id=\"p2\">");
    {
      uint32_t total = errorStatsTotal();
      uint32_t failed = errorStatsFailed();
      uint32_t hits = errorStatsCacheHits();
      char all[16];
      fmtCount(all, sizeof(all), total + hits);
      response->printf(
        "<div class=\"kpis\">"
          "<div class=\"kpi\"><small>Requests</small><b>%s</b></div>"
          "<div class=\"kpi\"><small>From cache</small><b>%.0f %%</b></div>"
          "<div class=\"kpi\"><small>Errors</small><b>%.2f %%</b></div>"
        "</div>",
        all, (hits + total) ? 100.0 * hits / (hits + total) : 0.0, total ? 100.0 * failed / total : 0.0);

      // Clients
      response->print("<h3>Clients</h3><table>");
      ClientInfo cl[16];
      uint8_t n = clientsGet(cl, 16);
      if (n == 0) sendTableRow(response, "No clients seen yet", String(""));
      for (uint8_t i = 0; i < n; i++) {
        char ip[16];
        snprintf(ip, sizeof(ip), "%s", IPAddress(cl[i].ip).toString().c_str());
        char since[24], seen[32];
        if (cl[i].firstEpoch) {
          struct tm lt;
          localtime_r(&cl[i].firstEpoch, &lt);
          strftime(since, sizeof(since), "%d.%m. %H:%M", &lt);
        } else {
          snprintf(since, sizeof(since), "boot + %lu s", (unsigned long)(cl[i].firstMs / 1000));
        }
        if (cl[i].active) {
          snprintf(seen, sizeof(seen), "connected");
        } else {
          uint32_t ago = (millis() - cl[i].lastMs) / 1000;
          if (ago < 3600) snprintf(seen, sizeof(seen), "seen %lu min ago", (unsigned long)(ago / 60));
          else            snprintf(seen, sizeof(seen), "seen %lu h ago", (unsigned long)(ago / 3600));
        }
        snprintf(buf, sizeof(buf), "<span class=\"%s\">%s</span><small>since %s &middot; %u conn.</small>",
          cl[i].active ? "lg" : "", seen, since, cl[i].connects);
        sendTableRow(response, ip, String(buf));
      }
      response->print("</table>");

      // Fehlerarten als Balken
      response->print("<h3>Error types</h3>");
      if (failed == 0) response->print("<p class=\"hint\">No errors recorded.</p>");
      for (int code = 1; code < 256; code++) {
        uint32_t cnt = errorStatsGet(code);
        if (cnt == 0) continue;
        snprintf(buf, sizeof(buf),
          "<div class=\"eb\"><span>%s</span><span class=\"bar\"><i class=\"r\" style=\"width:%.0f%%\"></i></span><span class=\"n\">%lu</span></div>",
          ErrorName((Modbus::Error)code).c_str(), 100.0 * cnt / failed, (unsigned long)cnt);
        response->print(buf);
      }

      // Fehlerprotokoll, eine Zeile pro Eintrag
      response->print("<h3>Recent errors</h3><table>");
      ErrorLogEntry entries[ERROR_LOG_SIZE];
      uint8_t ne = errorStatsLog(entries, ERROR_LOG_SIZE);
      if (ne == 0) sendTableRow(response, "None", String(""));
      time_t nowT = time(nullptr);
      struct tm today;
      localtime_r(&nowT, &today);
      for (uint8_t i = 0; i < ne; i++) {
        const ErrorLogEntry &en = entries[i];
        char when[24];
        if (en.epoch) {
          struct tm lt;
          localtime_r(&en.epoch, &lt);
          bool sameDay = lt.tm_yday == today.tm_yday && lt.tm_year == today.tm_year;
          strftime(when, sizeof(when), sameDay ? "%H:%M:%S" : "%d.%m. %H:%M", &lt);
        } else {
          snprintf(when, sizeof(when), "boot + %lu s", (unsigned long)(en.ms / 1000));
        }
        snprintf(buf, sizeof(buf), "%s &middot; ID %u &middot; 0x%04X<small>FC %02u &middot; %u registers</small>",
          ErrorName((Modbus::Error)en.code).c_str(), en.serverId, en.reg, en.fc, en.count);
        sendTableRow(response, when, String(buf));
      }
      response->print("</table><p></p>"
        "<form method=\"post\" action=\"stats/reset\"><button class=\"s\">Reset statistics</button></form>");

      // Rohzaehler der eModbus-Bibliothek
      response->print("<h3>Library counters</h3><table>");
      sendTableRow(response, "RTU messages / errors",
        String(rtu->getMessageCount()) + " / " + String(rtu->getErrorCount()));
      sendTableRow(response, "RTU pending", rtu->pendingRequests());
      sendTableRow(response, "Bridge messages / errors",
        String(bridge->getMessageCount()) + " / " + String(bridge->getErrorCount()));
      sendTableRow(response, "Bridge clients", bridge->activeClients());
      response->print("</table>");
    }
    response->print("</div>");

    // ======== Reiter 3: Netzwerk ========
    response->print("<div class=\"pane\" id=\"p3\"><table>");
    sendTableRow(response, "SSID", WiFi.SSID());
    snprintf(buf, sizeof(buf), "%d dBm &middot; %s", WiFi.RSSI(), WiFiQuality(WiFi.RSSI()).c_str());
    sendTableRow(response, "Signal", String(buf));
    sendTableRow(response, "IP", WiFi.localIP().toString());
    sendTableRow(response, "Gateway", WiFi.gatewayIP().toString());
    sendTableRow(response, "Hostname", String("modbus2eth.local"));
    sendTableRow(response, "MAC", WiFi.macAddress());
    sendTableRow(response, "Modbus TCP port", (uint32_t)config->getTcpPort());
    response->print("</table></div>");

    // ======== Reiter 4: System ========
    response->print("<div class=\"pane\" id=\"p4\"><h3>CPU load</h3>");
    {
      float worst = 0;
      for (uint8_t c = 0; c < cpuCores(); c++) {
        float avg = cpuLoadAvg(c);
        if (avg > worst) worst = avg;
        const char *cls = avg < 50 ? "g" : (avg < 80 ? "y" : "r");
        snprintf(buf, sizeof(buf),
          "<div class=\"eb\"><span>Core %u (%s)</span><span class=\"bar\"><i class=\"%s\" style=\"width:%.0f%%\"></i></span>"
          "<span class=\"n\">%.0f %%</span></div>",
          c, c == 0 ? "WiFi" : "App", cls, avg, avg);
        response->print(buf);
      }
      response->print("<table>");
      String verdict = worst < 50 ? "<span class=\"lg\">OK</span>"
                     : worst < 80 ? "<span class=\"ly\">Busy</span>"
                                  : "<span class=\"lr\">At the limit</span>";
      sendTableRow(response, "Assessment (1 min avg)", verdict);
      for (uint8_t c = 0; c < cpuCores(); c++) {
        snprintf(buf, sizeof(buf), "%.0f %% now &middot; %.0f %% peak", cpuLoadNow(c), cpuLoadPeak(c));
        char name[16];
        snprintf(name, sizeof(name), "Core %u", c);
        sendTableRow(response, name, String(buf));
      }
      response->print("</table>");
    }
    response->print("<h3>Memory</h3><table>");
    sendTableRow(response, "Free heap (bytes)", ESP.getFreeHeap());
    sendTableRow(response, "Lowest free heap", ESP.getMinFreeHeap());
    sendTableRow(response, "Largest free block", ESP.getMaxAllocHeap());
    response->print("</table><h3>Runtime</h3><table>");
    snprintf(buf, sizeof(buf), "%lu d %02u:%02u:%02u", (unsigned long)(s / 86400),
      (unsigned)((s % 86400) / 3600), (unsigned)((s % 3600) / 60), (unsigned)(s % 60));
    sendTableRow(response, "Uptime", String(buf));
    sendTableRow(response, "Last restart", healthLastRestart());
    sendTableRow(response, "Build time", __DATE__ " " __TIME__);
    response->print("</table></div><p></p>");

    sendButton(response, "Back", "/", "s");
    // Gewaehlten Reiter in der Adresse merken, damit er nach dem Neuladen erhalten bleibt
    response->print("<script>"
      "(function(){"
        "var h=location.hash.replace('#','');"
        "if(h&&document.getElementById(h))document.getElementById(h).checked=true;"
        "document.querySelectorAll('.tabr').forEach(function(r){"
          "r.addEventListener('change',function(){history.replaceState(null,'','#'+r.id);});"
        "});"
      "})();"
      "</script>");
    sendResponseTrailer(response);
    request->send(response);
  });

  server->on("/stats/reset", HTTP_POST, [config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    dbgln("[webserver] POST /stats/reset");
    errorStatsReset();
    cpuLoadResetPeak();
    request->redirect("/status#t2");   // zurueck zum Reiter Modbus
  });

  // Maschinenlesbare Werte, z. B. fuer Home Assistant (REST-Sensor)
  server->on("/api/status", HTTP_GET, [config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    auto *response = request->beginResponseStream("application/json");
    MeterValues mv = meterGet();
    response->printf("{\"uptime_s\":%llu,", (unsigned long long)(esp_timer_get_time() / 1000000));
    if (mv.valid) {
      response->printf("\"meter\":{\"age_s\":%lu,"
        "\"voltage\":[%.1f,%.1f,%.1f],\"current\":[%.2f,%.2f,%.2f],\"power\":[%.0f,%.0f,%.0f],"
        "\"total_power\":%.0f,\"frequency\":%.2f,\"import_kwh\":%.2f,\"export_kwh\":%.2f,\"total_kwh\":%.2f},",
        (unsigned long)((millis() - mv.updatedMs) / 1000),
        mv.voltage[0], mv.voltage[1], mv.voltage[2],
        mv.current[0], mv.current[1], mv.current[2],
        mv.power[0], mv.power[1], mv.power[2],
        mv.totalPower, mv.frequency, mv.importKwh, mv.exportKwh, mv.totalKwh);
    } else {
      response->print("\"meter\":null,");
    }
    response->printf("\"stats\":{\"bus_requests\":%lu,\"cache_hits\":%lu,\"failed\":%lu},",
      (unsigned long)errorStatsTotal(), (unsigned long)errorStatsCacheHits(), (unsigned long)errorStatsFailed());
    response->print("\"cpu\":[");
    for (uint8_t c = 0; c < cpuCores(); c++) {
      response->printf("%s{\"now\":%.0f,\"avg\":%.0f,\"peak\":%.0f}", c ? "," : "",
        cpuLoadNow(c), cpuLoadAvg(c), cpuLoadPeak(c));
    }
    response->print("],");
    {
      ClientInfo cl[16];
      uint8_t n = clientsGet(cl, 16);
      response->print("\"clients\":[");
      for (uint8_t i = 0; i < n; i++) {
        response->printf("%s{\"ip\":\"%s\",\"active\":%u,\"connections\":%u}", i ? "," : "",
          IPAddress(cl[i].ip).toString().c_str(), cl[i].active, cl[i].connects);
      }
      response->print("],");
    }
    response->printf("\"heap\":{\"free\":%lu,\"min_free\":%lu},",
      (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
    response->printf("\"rssi\":%d,\"last_restart\":\"%s\"}", WiFi.RSSI(), healthLastRestart().c_str());
    request->send(response);
  });

  server->on("/reboot", HTTP_GET, [config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    dbgln("[webserver] GET /reboot");
    auto *response = request->beginResponseStream("text/html");
    sendResponseHeader(response, "Reboot");
    response->print("<p>Restart the gateway now? Modbus TCP clients will be disconnected for a few seconds.</p>"
      "<form method=\"post\">"
        "<button class=\"r\">Reboot now</button>"
      "</form><p></p>");
    sendButton(response, "Back", "/", "s");
    sendResponseTrailer(response);
    request->send(response);
  });
  server->on("/reboot", HTTP_POST, [config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    dbgln("[webserver] POST /reboot");
    request->redirect("/");
    dbgln("[webserver] rebooting...")
    healthRestart("Reboot via web interface");
  });

  server->on("/config", HTTP_GET, [config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    dbgln("[webserver] GET /config");
    auto *response = request->beginResponseStream("text/html");
    sendResponseHeader(response, "Config");
    response->print("<form method=\"post\">"
      "<h3>Modbus TCP</h3>"
      "<table>"
      "<tr><td><label for=\"tp\">TCP port</label></td><td>");
    response->printf("<input type=\"number\" min=\"1\" max=\"65535\" id=\"tp\" name=\"tp\" value=\"%d\">", config->getTcpPort());
    response->print("</td></tr>"
      "<tr><td><label for=\"tt\">TCP timeout (ms)</label></td><td>");
    response->printf("<input type=\"number\" min=\"1\" id=\"tt\" name=\"tt\" value=\"%d\">", config->getTcpTimeout());
    response->print("</td></tr>"
      "</table>"
      "<h3>Modbus RTU</h3>"
      "<table>"
      "<tr><td><label for=\"mb\">Baud rate</label></td><td>");
    response->printf("<input type=\"number\" min=\"0\" id=\"mb\" name=\"mb\" value=\"%lu\">", config->getModbusBaudRate());
    response->print("</td></tr>"
      "<tr><td><label for=\"md\">Data bits</label></td><td>");
    response->printf("<input type=\"number\" min=\"5\" max=\"8\" id=\"md\" name=\"md\" value=\"%d\">", config->getModbusDataBits());
    response->print("</td></tr>"
      "<tr><td><label for=\"mp\">Parity</label></td><td>");
    response->printf("<select id=\"mp\" name=\"mp\" data-value=\"%d\">", config->getModbusParity());
    response->print("<option value=\"0\">None</option>"
        "<option value=\"2\">Even</option>"
        "<option value=\"3\">Odd</option>"
      "</select></td></tr>"
      "<tr><td><label for=\"ms\">Stop bits</label></td><td>");
    response->printf("<select id=\"ms\" name=\"ms\" data-value=\"%d\">", config->getModbusStopBits());
    response->print("<option value=\"1\">1 bit</option>"
        "<option value=\"2\">1.5 bits</option>"
        "<option value=\"3\">2 bits</option>"
      "</select></td></tr>"
      "<tr><td><label for=\"mr\">RTS pin</label></td><td>");
    response->printf("<select id=\"mr\" name=\"mr\" data-value=\"%d\">", config->getModbusRtsPin());
    response->print("<option value=\"-1\">Auto</option>"
        "<option value=\"4\">D4</option>"
        "<option value=\"13\">D13</option>"
        "<option value=\"14\">D14</option>"
        "<option value=\"18\">D18</option>"
        "<option value=\"19\">D19</option>"
        "<option value=\"21\">D21</option>"
        "<option value=\"22\">D22</option>"
        "<option value=\"23\">D23</option>"
        "<option value=\"25\">D25</option>"
        "<option value=\"26\">D26</option>"
        "<option value=\"27\">D27</option>"
        "<option value=\"32\">D32</option>"
        "<option value=\"33\">D33</option>"
      "</select></td></tr>"
      "</table>"
      "<h3>Serial (debug)</h3>"
      "<table>"
      "<tr><td><label for=\"sb\">Baud rate</label></td><td>");
    response->printf("<input type=\"number\" min=\"0\" id=\"sb\" name=\"sb\" value=\"%lu\">", config->getSerialBaudRate());
    response->print("</td></tr>"
      "<tr><td><label for=\"sd\">Data bits</label></td><td>");
    response->printf("<input type=\"number\" min=\"5\" max=\"8\" id=\"sd\" name=\"sd\" value=\"%d\">", config->getSerialDataBits());
    response->print("</td></tr>"
      "<tr><td><label for=\"sp\">Parity</label></td><td>");
    response->printf("<select id=\"sp\" name=\"sp\" data-value=\"%d\">", config->getSerialParity());
    response->print("<option value=\"0\">None</option>"
        "<option value=\"2\">Even</option>"
        "<option value=\"3\">Odd</option>"
      "</select></td></tr>"
      "<tr><td><label for=\"ss\">Stop bits</label></td><td>");
    response->printf("<select id=\"ss\" name=\"ss\" data-value=\"%d\">", config->getSerialStopBits());
    response->print("<option value=\"1\">1 bit</option>"
        "<option value=\"2\">1.5 bits</option>"
        "<option value=\"3\">2 bits</option>"
      "</select></td></tr>"
      "</table>"
      "<h3>Access</h3>"
      "<table>"
      "<tr><td><label for=\"wp\">Web password</label></td><td>");
    // Das echte Passwort wird nie ausgeliefert, nur ein Platzhalter
    response->printf("<input type=\"password\" id=\"wp\" name=\"wp\" value=\"%s\">", WEB_PASS_PLACEHOLDER);
    response->print("</td></tr>"
      "</table>"
      "<p class=\"hint\">Changes take effect after a reboot. Leave the password empty to disable the login.</p>"
      "<button>Save settings</button>"
      "</form><p></p>");
    sendButton(response, "Back", "/", "s");
    response->print(SELECT_SCRIPT);
    sendResponseTrailer(response);
    request->send(response);
  });
  server->on("/config", HTTP_POST, [config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    dbgln("[webserver] POST /config");
    if (request->hasParam("tp", true)){
      auto port = request->getParam("tp", true)->value().toInt();
      config->setTcpPort(port);
      dbgln("[webserver] saved port");
    }
    if (request->hasParam("tt", true)){
      auto timeout = request->getParam("tt", true)->value().toInt();
      config->setTcpTimeout(timeout);
      dbgln("[webserver] saved timeout");
    }
    if (request->hasParam("mb", true)){
      auto baud = request->getParam("mb", true)->value().toInt();
      config->setModbusBaudRate(baud);
      dbgln("[webserver] saved modbus baud rate");
    }
    if (request->hasParam("md", true)){
      auto data = request->getParam("md", true)->value().toInt();
      config->setModbusDataBits(data);
      dbgln("[webserver] saved modbus data bits");
    }
    if (request->hasParam("mp", true)){
      auto parity = request->getParam("mp", true)->value().toInt();
      config->setModbusParity(parity);
      dbgln("[webserver] saved modbus parity");
    }
    if (request->hasParam("ms", true)){
      auto stop = request->getParam("ms", true)->value().toInt();
      config->setModbusStopBits(stop);
      dbgln("[webserver] saved modbus stop bits");
    }
    if (request->hasParam("mr", true)){
      auto rts = request->getParam("mr", true)->value().toInt();
      config->setModbusRtsPin(rts);
      dbgln("[webserver] saved modbus rts pin");
    }
    if (request->hasParam("sb", true)){
      auto baud = request->getParam("sb", true)->value().toInt();
      config->setSerialBaudRate(baud);
      dbgln("[webserver] saved serial baud rate");
    }
    if (request->hasParam("sd", true)){
      auto data = request->getParam("sd", true)->value().toInt();
      config->setSerialDataBits(data);
      dbgln("[webserver] saved serial data bits");
    }
    if (request->hasParam("sp", true)){
      auto parity = request->getParam("sp", true)->value().toInt();
      config->setSerialParity(parity);
      dbgln("[webserver] saved serial parity");
    }
    if (request->hasParam("ss", true)){
      auto stop = request->getParam("ss", true)->value().toInt();
      config->setSerialStopBits(stop);
      dbgln("[webserver] saved serial stop bits");
    }
    if (request->hasParam("wp", true)){
      String wp = request->getParam("wp", true)->value();
      if (!wp.equals(WEB_PASS_PLACEHOLDER)) {   // Platzhalter = Passwort unveraendert
        config->setWebPassword(wp);
        dbgln("[webserver] saved web password");
      } else {
        dbgln("[webserver] web password not changed");
      }
    }
    request->redirect("/");
  });

  server->on("/pins", HTTP_GET, [config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    dbgln("[webserver] GET /pins");
    auto *response = request->beginResponseStream("text/html");
    sendResponseHeader(response, "Pins & IP");
    response->print("<form method=\"post\">"
      "<h3>RS485</h3>"
      "<table>"
      "<tr><td><label for=\"tx\">TX pin</label></td><td>");
    response->printf("<input type=\"number\" min=\"0\" max=\"33\" id=\"tx\" name=\"tx\" value=\"%d\">", config->gettxpin());
    response->print("</td></tr>"
      "<tr><td><label for=\"rx\">RX pin</label></td><td>");
    response->printf("<input type=\"number\" min=\"0\" max=\"39\" id=\"rx\" name=\"rx\" value=\"%d\">", config->getrxpin());
    response->print("</td></tr>"
      "</table>"
      "<h3>Network</h3>"
      "<table>"
      "<tr><td><label for=\"ipa\">IP address</label></td><td>");
    IP_PLACEHOLDER = config->getipAdr();
    response->printf("<input type=\"text\" inputmode=\"decimal\" id=\"ipa\" name=\"ipa\" value=\"%s\">", IP_PLACEHOLDER.c_str());
    response->print("</td></tr>"
      "</table>"
      "<p class=\"hint\">GPIO 6-11 are reserved for flash. TX needs an output-capable pin (0-33). Changes take effect after a reboot.</p>"
      "<button>Save pins and IP</button>"
      "</form><p></p>");
    sendButton(response, "Back", "/", "s");
    sendResponseTrailer(response);
    request->send(response);
  });
  server->on("/pins", HTTP_POST, [config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    dbgln("[webserver] POST /pins");
    if (request->hasParam("tx", true)){
      auto pintx = request->getParam("tx", true)->value().toInt();
      if (validPin(pintx, true)) {
        config->settxpin(pintx);
        dbgln("[webserver] saved Pin TX");
      } else {
        dbgln("[webserver] invalid Pin TX ignored");
      }
    }
    if (request->hasParam("rx", true)){
      auto pinrx = request->getParam("rx", true)->value().toInt();
      if (validPin(pinrx, false)) {
        config->setrxpin(pinrx);
        dbgln("[webserver] saved Pin RX");
      } else {
        dbgln("[webserver] invalid Pin RX ignored");
      }
    }
    if (request->hasParam("ipa", true)){
      String ipa = request->getParam("ipa", true)->value();
      if (!ipa.equals(IP_PLACEHOLDER)) {
        config->setipAdr(ipa);
        dbgln("[webserver] saved ip Adress");
      } else {
        dbgln("[webserver] ip adress not changed");
      }
    }
    request->redirect("/");
  });

  server->on("/debug", HTTP_GET, [config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    dbgln("[webserver] GET /debug");
    auto *response = request->beginResponseStream("text/html");
    sendResponseHeader(response, "Debug");
    sendDebugForm(response, "1", "1", "3", "1");
    sendButton(response, "Back", "/", "s");
    sendResponseTrailer(response);
    request->send(response);
  });
  server->on("/debug", HTTP_POST, [rtu, config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    dbgln("[webserver] POST /debug");
    String slaveId = "1";
    if (request->hasParam("slave", true)){
      slaveId = request->getParam("slave", true)->value();
    }
    String reg = "1";
    if (request->hasParam("reg", true)){
      reg = request->getParam("reg", true)->value();
    }
    String func = "3";
    if (request->hasParam("func", true)){
      func = request->getParam("func", true)->value();
    }
    String count = "1";
    if (request->hasParam("count", true)){
      count = request->getParam("count", true)->value();
    }
    auto *response = request->beginResponseStream("text/html");
    sendResponseHeader(response, "Debug");
    response->print("<pre>");
    auto previous = LOGDEVICE;
    auto previousLevel = MBUlogLvl;
    auto debug = WebPrint(previous, response);
    LOGDEVICE = &debug;
    MBUlogLvl = LOG_LEVEL_DEBUG;
    ModbusMessage answer = rtu->syncRequest(0xdeadbeef, slaveId.toInt(), func.toInt(), reg.toInt(), count.toInt());
    MBUlogLvl = previousLevel;
    LOGDEVICE = previous;
    response->print("</pre>");
    auto error = answer.getError();
    if (error == SUCCESS){
      auto bytes = answer[2];
      response->print("<p class=\"ok\">Answer: 0x");
      for (size_t i = 0; i < bytes; i++)
      {
        response->printf("%02x", answer[i + 3]);
      }
      response->print("</p>");
    }
    else{
      response->printf("<p class=\"e\">Error: %#02x (%s)</p>", error, ErrorName(error).c_str());
    }
    sendDebugForm(response, slaveId, reg, func, count);
    sendButton(response, "Back", "/", "s");
    sendResponseTrailer(response);
    request->send(response);
  });

  server->on("/update", HTTP_GET, [config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    dbgln("[webserver] GET /update");
    auto *response = request->beginResponseStream("text/html");
    sendResponseHeader(response, "Firmware update");
    response->print("<form method=\"post\" enctype=\"multipart/form-data\">"
      "<p class=\"hint\">Select a firmware .bin file. The gateway restarts after the upload.</p>"
      "<input type=\"file\" name=\"file\" id=\"file\" required/>"
      "<p></p>"
      "<button class=\"r\">Upload firmware</button>"
      "</form><p></p>");
    sendButton(response, "Back", "/", "s");
    sendResponseTrailer(response);
    request->send(response);
  });
  server->on("/update", HTTP_POST, [config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    request->onDisconnect([](){
      healthRestart("Firmware update via web interface");
    });
    dbgln("[webserver] OTA finished");
    if (Update.hasError()){
      auto *response = request->beginResponse(500, "text/plain", "Ota failed");
      response->addHeader("Connection", "close");
      request->send(response);
    }
    else{
      auto *response = request->beginResponseStream("text/html");
      response->addHeader("Connection", "close");
      sendResponseHeader(response, "Firmware update", true);
      response->print("<p class=\"ok\">Update successful. The gateway is restarting.</p>");
      sendButton(response, "Back", "/", "s");
      sendResponseTrailer(response);
      request->send(response);
    }
  }, [config](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
    ADMIN_WEB_PASS;
    dbg("[webserver] OTA progress ");dbgln(index);
    if (!index) {
      //TODO add MD5 Checksum and Update.setMD5
      int cmd = (filename == "filesystem") ? U_SPIFFS : U_FLASH;
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) { // Start with max available size
        Update.printError(Serial);
        return request->send(400, "text/plain", "OTA could not begin");
      }
    }
    if(len){
      if (Update.write(data, len) != len) {
        return request->send(400, "text/plain", "OTA could not write data");
      }
    }
    if (final) {
      if (!Update.end(true)) {
        Update.printError(Serial);
        return request->send(400, "text/plain", "Could not end OTA");
      }
    }else{
      return;
    }
  });

  server->on("/wifi", HTTP_GET, [config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    dbgln("[webserver] GET /wifi");
    auto *response = request->beginResponseStream("text/html");
    sendResponseHeader(response, "WiFi reset");
    response->print("<p class=\"e\">"
        "This deletes the stored WiFi credentials and restarts the gateway as an access point. "
        "You will need to connect to it and enter the WiFi details again."
      "</p>"
      "<form method=\"post\">"
        "<button class=\"r\">Delete WiFi and restart</button>"
      "</form><p></p>");
    sendButton(response, "Back", "/", "s");
    sendResponseTrailer(response);
    request->send(response);
  });
  server->on("/wifi", HTTP_POST, [wm, config](AsyncWebServerRequest *request){
    ADMIN_WEB_PASS;
    dbgln("[webserver] POST /wifi");
    request->redirect("/");
    wm->erase();
    dbgln("[webserver] erased wifi config");
    dbgln("[webserver] rebooting...");
    healthRestart("WiFi reset via web interface");
  });

  server->on("/favicon.ico", [](AsyncWebServerRequest *request){
    dbgln("[webserver] GET /favicon.ico");
    request->send(204);
  });
  server->on("/style.css", [config](AsyncWebServerRequest *request){
    if (request->hasHeader("If-None-Match")){
      auto header = request->getHeader("If-None-Match");
      if (header->value() == String(ETAG)){
        request->send(304);
        return;
      }
    }
    dbgln("[webserver] GET /style.css");
    auto *response = request->beginResponseStream("text/css");
    sendMinCss(response);
    response->addHeader("ETag", ETAG);
    request->send(response);
  });
  server->onNotFound([config](AsyncWebServerRequest *request){
    dbg("[webserver] request to ");dbg(request->url());dbgln(" not found");
    request->send(404, "text/plain", "404");
  });
}

// Komplettes Stylesheet: wird als style.css ausgeliefert und auf der Update-Seite inline eingebettet
// Design: Ferraris-Zaehler (Zifferblatt, Rollenzaehlwerk, Zaehlerscheibe)
void sendMinCss(AsyncResponseStream *response){
  response->print(
    ":root{"
      "--bg:#2a2624;--face:#ecebe4;--rim:#d8d6cc;--ink:#1c1a19;--muted:#55504c;--line:#cfccc1;"
      "--btnh:#3a3532;--red:#b3261e;--redh:#8f1e18;--ok:#2f6b3a;--field:#faf9f4;"
    "}"
    "*{box-sizing:border-box}"
    "body{margin:0;padding:22px 12px;background:var(--bg);color:var(--ink);"
      "font:16px/1.45 \"Gill Sans\",\"Trebuchet MS\",system-ui,sans-serif}"
    "#content{max-width:440px;margin:0 auto;background:var(--face);border-radius:14px;"
      "padding:20px 18px;box-shadow:inset 0 0 0 6px var(--rim)}"
    ".top small{display:flex;justify-content:space-between;gap:12px;font-size:.78rem;color:var(--muted)}"
    ".top h1{margin:12px 0 16px;text-align:center;font-size:1.4rem;font-weight:600;letter-spacing:.04em}"
    ".roll{display:flex;justify-content:center;gap:4px;margin-top:4px}"
    ".d{width:34px;height:48px;display:flex;align-items:center;justify-content:center;border-radius:3px;"
      "background:var(--ink);color:#f4f2ea;font:30px Georgia,serif;"
      "box-shadow:inset 0 7px 6px -5px rgba(255,255,255,.22),inset 0 -7px 6px -5px rgba(0,0,0,.7)}"
    ".d.r{background:var(--red);color:#fff}"
    ".c{width:10px;align-self:center;text-align:center;font-size:26px}"
    ".cap{text-align:center;font-size:.78rem;color:var(--muted);margin:6px 0 14px}"
    ".disc{position:relative;height:10px;border-radius:5px;background:#bfc3c6;overflow:hidden;margin-bottom:6px;"
      "box-shadow:inset 0 1px 2px rgba(0,0,0,.25)}"
    ".disc i{position:absolute;top:0;left:-24px;width:22px;height:100%;background:var(--red);"
      "animation:spin 6s linear infinite}"
    "@keyframes spin{to{left:100%}}"
    "@media (prefers-reduced-motion:reduce){.disc i{animation:none;left:38%}}"
    "h3{margin:22px 0 4px;padding-bottom:4px;font-size:.95rem;font-weight:600;color:var(--muted);"
      "border-bottom:1px solid var(--line)}"
    "form{margin:0}"
    "p{margin:8px 0}"
    "button{display:block;width:100%;padding:12px 14px;border:0;border-radius:10px;background:var(--ink);"
      "color:var(--face);font:inherit;font-weight:600;cursor:pointer}"
    "button:hover{background:var(--btnh)}"
    "button.r{background:var(--red);color:#fff}"
    "button.r:hover{background:var(--redh)}"
    "button.s{background:transparent;color:var(--ink);border:1px solid #8f8983;font-weight:500}"
    "button.s:hover{background:var(--rim)}"
    "button:focus-visible,input:focus-visible,select:focus-visible{outline:3px solid #4d74b8;outline-offset:2px}"
    "hr{border:0;border-top:1px solid var(--line);margin:18px 0 14px}"
    "table{width:100%;border-collapse:collapse}"
    "td{padding:7px 0;border-bottom:1px solid var(--line);vertical-align:middle}"
    "td:first-child{width:48%;padding-right:12px;color:var(--muted)}"
    "td:last-child{text-align:right;font-variant-numeric:tabular-nums;word-break:break-all}"
    "td small{display:block;color:var(--muted);font-size:.78rem}"
    ".bar{display:block;height:6px;margin-top:4px;background:var(--line);border-radius:3px;overflow:hidden}"
    ".bar i{display:block;height:100%;min-width:2px}"
    ".bar i.g{background:#4f8a4a}.bar i.y{background:#c9962b}.bar i.r{background:var(--red)}"
    ".lg{color:#3d7a38;font-weight:600}.ly{color:#a6761b;font-weight:600}.lr{color:var(--red);font-weight:600}"
    "#content{position:relative}"
    ".top.st{display:flex;justify-content:space-between;align-items:center;gap:10px;margin-bottom:12px}"
    ".top.st small{display:block}"
    ".top.st h1{margin:2px 0 0;text-align:left}"
    ".roll.mini{gap:2px;margin:0}"
    ".roll.mini .d{width:17px;height:26px;font-size:16px;border-radius:2px}"
    ".roll.mini .c{width:5px;font-size:14px}"
    ".tabr{position:absolute;opacity:0;pointer-events:none}"
    ".tabs{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:3px;padding:3px;margin:14px 0 6px;"
      "background:var(--rim);border-radius:10px}"
    ".tabs label{padding:8px 0;border-radius:8px;text-align:center;font-size:.85rem;color:var(--muted);cursor:pointer}"
    "#t1:checked~.tabs label[for=t1],#t2:checked~.tabs label[for=t2],"
    "#t3:checked~.tabs label[for=t3],#t4:checked~.tabs label[for=t4]"
      "{background:var(--ink);color:var(--face);font-weight:600}"
    "#t1:focus-visible~.tabs label[for=t1],#t2:focus-visible~.tabs label[for=t2],"
    "#t3:focus-visible~.tabs label[for=t3],#t4:focus-visible~.tabs label[for=t4]"
      "{outline:3px solid #4d74b8;outline-offset:1px}"
    ".pane{display:none}"
    "#t1:checked~#p1,#t2:checked~#p2,#t3:checked~#p3,#t4:checked~#p4{display:block}"
    ".big{text-align:center;margin:12px 0 10px}"
    ".big b{display:block;font-size:2.2rem;font-weight:600;line-height:1.1;font-variant-numeric:tabular-nums}"
    ".big span{font-size:.85rem;color:var(--muted)}"
    ".ph{display:grid;grid-template-columns:34px repeat(3,minmax(0,1fr));gap:4px;padding:6px 0;"
      "border-bottom:1px solid var(--line);font-variant-numeric:tabular-nums}"
    ".ph span:first-child{color:var(--muted)}"
    ".ph span:not(:first-child){text-align:right}"
    ".ph.hd{font-size:.78rem;color:var(--muted);padding-top:0}"
    ".kpis{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px;margin-top:10px}"
    ".kpi{background:#f7f6f1;border:1px solid var(--line);border-radius:8px;padding:8px}"
    ".kpi small{display:block;font-size:.78rem;color:var(--muted)}"
    ".kpi b{font-size:1.2rem;font-weight:600}"
    ".eb{display:flex;align-items:center;gap:10px;padding:7px 0}"
    ".eb span:first-child{width:42%}"
    ".eb .bar{flex-grow:1;margin:0;height:8px;border-radius:4px}"
    ".eb .n{width:3.5em;text-align:right;font-variant-numeric:tabular-nums}"
    "input,select{width:100%;padding:7px 9px;border:1px solid #bdb9ad;border-radius:6px;background:var(--field);"
      "color:var(--ink);font:inherit;text-align:right}"
    "input[type=file]{text-align:left;padding:6px}"
    "table+button,.hint+button{margin-top:16px}"
    ".hint{font-size:.85rem;color:var(--muted)}"
    ".e{color:var(--red)}"
    ".ok{color:var(--ok);font-family:ui-monospace,Menlo,Consolas,monospace;word-break:break-all}"
    "pre{white-space:pre-wrap;font-size:.78rem;background:var(--field);border:1px solid var(--line);"
      "border-radius:6px;padding:8px;margin:0 0 8px;overflow-x:auto}"
    "pre:empty{display:none}"
  );
}

// Dokumentkopf bis einschliesslich <div id="content">
static void sendHead(AsyncResponseStream *response, const char *title, bool inlineStyle){
    response->print("<!DOCTYPE html>"
      "<html lang=\"en\">"
      "<head>"
      "<meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"/>");
    response->printf("<title>Modbus Gateway - %s</title>", title);
    if (inlineStyle){
      response->print("<style>");
      sendMinCss(response);
      response->print("</style>");
    }
    else{
      response->print("<link rel=\"stylesheet\" href=\"style.css\">");
    }
    response->print("</head><body><div id=\"content\">");
}

void sendResponseHeader(AsyncResponseStream *response, const char *title, bool inlineStyle){
    sendHead(response, title, inlineStyle);
    response->print("<header class=\"top\">");
    response->printf("<small><span>Modbus Gateway</span><span>%s</span></small>", WiFi.localIP().toString().c_str());
    response->printf("<h1>%s</h1></header>", title);
}

// Grosse Zahlen kompakt: 9876, 21.5 k, 1.23 M
static void fmtCount(char *out, size_t len, uint32_t v){
    if (v < 10000)        snprintf(out, len, "%lu", (unsigned long)v);
    else if (v < 1000000) snprintf(out, len, "%.1f k", v / 1000.0);
    else                  snprintf(out, len, "%.2f M", v / 1000000.0);
}

void sendResponseTrailer(AsyncResponseStream *response){
    response->print("</div></body></html>");
}

void sendButton(AsyncResponseStream *response, const char *title, const char *action, const char *css){
    response->printf(
      "<form method=\"get\" action=\"%s\">"
        "<button class=\"%s\">%s</button>"
      "</form>"
      "<p></p>", action, css, title);
}

void sendTableRow(AsyncResponseStream *response, const char *name, String value){
    response->printf(
      "<tr>"
        "<td>%s</td>"
        "<td>%s</td>"
      "</tr>", name, value.c_str());
}

void sendTableRow(AsyncResponseStream *response, const char *name, uint32_t value){
    response->printf(
      "<tr>"
        "<td>%s</td>"
        "<td>%u</td>"
      "</tr>", name, value);
}

void sendDebugForm(AsyncResponseStream *response, String slaveId, String reg, String function, String count){
    response->print("<form method=\"post\">"
      "<h3>Read request</h3>"
      "<table>"
      "<tr><td><label for=\"slave\">Slave ID</label></td><td>");
    response->printf("<input type=\"number\" min=\"0\" max=\"247\" id=\"slave\" name=\"slave\" value=\"%s\">", slaveId.c_str());
    response->print("</td></tr>"
      "<tr><td><label for=\"func\">Function</label></td><td>");
    response->printf("<select id=\"func\" name=\"func\" data-value=\"%s\">", function.c_str());
    response->print("<option value=\"1\">01 Read coils</option>"
        "<option value=\"2\">02 Read discrete inputs</option>"
        "<option value=\"3\">03 Read holding registers</option>"
        "<option value=\"4\">04 Read input registers</option>"
      "</select></td></tr>"
      "<tr><td><label for=\"reg\">Register</label></td><td>");
    response->printf("<input type=\"number\" min=\"0\" max=\"65535\" id=\"reg\" name=\"reg\" value=\"%s\">", reg.c_str());
    response->print("</td></tr>"
      "<tr><td><label for=\"count\">Count</label></td><td>");
    response->printf("<input type=\"number\" min=\"0\" max=\"65535\" id=\"count\" name=\"count\" value=\"%s\">", count.c_str());
    response->print("</td></tr>"
      "</table>"
      "<button>Send request</button>"
      "</form><p></p>");
    response->print(SELECT_SCRIPT);
}

const String ErrorName(Modbus::Error code)
{
    switch (code)
    {
        case Modbus::Error::SUCCESS: return "Success";
        case Modbus::Error::ILLEGAL_FUNCTION: return "Illegal function";
        case Modbus::Error::ILLEGAL_DATA_ADDRESS: return "Illegal data address";
        case Modbus::Error::ILLEGAL_DATA_VALUE: return "Illegal data value";
        case Modbus::Error::SERVER_DEVICE_FAILURE: return "Server device failure";
        case Modbus::Error::ACKNOWLEDGE: return "Acknowledge";
        case Modbus::Error::SERVER_DEVICE_BUSY: return "Server device busy";
        case Modbus::Error::NEGATIVE_ACKNOWLEDGE: return "Negative acknowledge";
        case Modbus::Error::MEMORY_PARITY_ERROR: return "Memory parity error";
        case Modbus::Error::GATEWAY_PATH_UNAVAIL: return "Gateway path unavailable";
        case Modbus::Error::GATEWAY_TARGET_NO_RESP: return "Gateway target no response";
        case Modbus::Error::TIMEOUT: return "Timeout";
        case Modbus::Error::INVALID_SERVER: return "Invalid server";
        case Modbus::Error::CRC_ERROR: return "CRC error";
        case Modbus::Error::FC_MISMATCH: return "Function code mismatch";
        case Modbus::Error::SERVER_ID_MISMATCH: return "Server id mismatch";
        case Modbus::Error::PACKET_LENGTH_ERROR: return "Packet length error";
        case Modbus::Error::PARAMETER_COUNT_ERROR: return "Parameter count error";
        case Modbus::Error::PARAMETER_LIMIT_ERROR: return "Parameter limit error";
        case Modbus::Error::REQUEST_QUEUE_FULL: return "Request queue full";
        case Modbus::Error::ILLEGAL_IP_OR_PORT: return "Illegal ip or port";
        case Modbus::Error::IP_CONNECTION_FAILED: return "IP connection failed";
        case Modbus::Error::TCP_HEAD_MISMATCH: return "TCP header mismatch";
        case Modbus::Error::EMPTY_MESSAGE: return "Empty message";
        case Modbus::Error::ASCII_FRAME_ERR: return "ASCII frame error";
        case Modbus::Error::ASCII_CRC_ERR: return "ASCII crc error";
        case Modbus::Error::ASCII_INVALID_CHAR: return "ASCII invalid character";
        default: return "undefined error";
    }
}

// RSSI in Qualitaetsstufe uebersetzen
const String WiFiQuality(int rssiValue)
{
    switch (rssiValue)
    {
        case -30 ... 0: return "Amazing";
        case -67 ... -31: return "Very good";
        case -70 ... -68: return "Okay";
        case -80 ... -71: return "Not good";
        default: return "Unusable";
    }
}
