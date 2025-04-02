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
#include <ESPmDNS.h>

AsyncWebServer webServer(80);
Config config;
Preferences prefs;
ModbusClientRTU *MBclient;
ModbusBridgeWiFi MBbridge;
WiFiManager wm;
// Set your Static IP address
IPAddress local_IP(192, 168, 207, 165);
// Set your Gateway IP address
IPAddress gateway(192, 168, 207, 1);
IPAddress subnet(255, 255, 255, 0);
//IPAddress primaryDNS(8, 8, 8, 8);   //optional
//IPAddress secondaryDNS(8, 8, 4, 4); //optional
String ipr; 

void setup() {
  debugSerial.begin(115200);
  dbgln();
  dbgln("[config] load")
  prefs.begin("modbusRtuGw");
  config.begin(&prefs);
  debugSerial.end();
  debugSerial.begin(config.getSerialBaudRate(), config.getSerialConfig());
  dbgln("[wifi] start");
  
  // WiFi.mode(WIFI_STA);
  // Configures static IP address
  ipr=config.getipAdr();
  local_IP.fromString(ipr);
  if (!WiFi.config(local_IP, gateway, subnet)) {
    Serial.println("STA Failed to configure");
  }
  wm.setClass("invert");
  auto reboot = false;
  wm.setAPCallback([&reboot](WiFiManager *wifiManager){reboot = true;});
  wm.autoConnect();
  String hostname = "ModbusSDM630toLAN";
  WiFi.setHostname(hostname.c_str()); //define hostname
  if (reboot){
    ESP.restart();
  }
  dbgln("[wifi] finished");
  dbgln("[modbus] start");

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

  MBclient = new ModbusClientRTU(config.getModbusRtsPin());
  MBclient->setTimeout(1000);
  MBclient->begin(modbusSerial, 1);
  for (uint8_t i = 1; i < 248; i++)
  {
    MBbridge.attachServer(i, i, ANY_FUNCTION_CODE, MBclient);
  }  
  MBbridge.start(config.getTcpPort(), 10, config.getTcpTimeout());
  dbgln("[modbus] finished");
  setupPages(&webServer, MBclient, &MBbridge, &config, &wm);
  webServer.begin();
  dbgln("[setup] finished");
  // Initialize mDNS
if (!MDNS.begin("modbus2")) {   // Set the hostname to "modbus2eth.local"
  Serial.println("Error setting up MDNS responder!");
  while(1) {
    delay(1000);
  }
}
}

void loop() {
  // put your main code here, to run repeatedly:
}
