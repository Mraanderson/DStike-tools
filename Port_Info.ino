/*
  ESP32-POE PortDiag (v0.4)
  - Stage 1: LLDP (Switch, Port, Mgmt IP, VLAN from LLDP)
  - Stage 2: DHCP (MyIP, MyVLAN=Unknown for now, Gateway, DNS)
  - Stage 3: Internet (reachability + latency)
  - Stage 4: Diagnostics (frames, LLDP count, last Ethertype)
  - Button (GPIO34) cycles stages: LLDP -> DHCP -> Internet -> Diag -> LLDP
  - AP always on: SSID "PortDiag", IP 192.168.4.1
  - Web:
      /           -> simple HTML status page
      /api/status -> JSON with all fields
*/

#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_eth.h"
#include "esp_bt.h"

// OLED pins
#define OLED_SDA 13
#define OLED_SCL 16
#define BUTTON_PIN 34

// Version
#define FW_VERSION "v0.4"

// OLED
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// Web
WebServer server(80);

// Button / UI state
volatile bool buttonPressed = false;

enum UIMode {
  MODE_LLDP = 0,
  MODE_DHCP,
  MODE_INTERNET,
  MODE_DIAG
};

UIMode uiMode = MODE_LLDP;

// Ethernet / LLDP state
volatile uint32_t frameCount = 0;
volatile uint32_t lldpCount = 0;
volatile uint16_t lastEthertype = 0;
bool linkUp = false;
bool promiscOn = false;
bool lldpSeen = false;

// Parsed LLDP fields
String sysName = "Unknown";
String portId  = "Unknown";
String mgmtIp  = "Unknown";
String vlanId  = "Unknown";

// DHCP / IP info
String myIp      = "0.0.0.0";
String myGateway = "0.0.0.0";
String myDns     = "0.0.0.0";
String myVlan    = "Unknown";   // placeholder until we parse DHCP options
bool   dhcpOk    = false;
bool   dhcpTestDone = false;

// Internet test
bool   internetOk       = false;
bool   internetTestDone = false;
uint32_t internetLatencyMs = 0;

// ---------------- Helpers ----------------
String ipv4ToStr(const uint8_t *p) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
  return String(buf);
}

String ipToStr(IPAddress ip) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return String(buf);
}

void IRAM_ATTR buttonISR() {
  buttonPressed = true;
}

// ---------------- OLED primitives ----------------
void oledClear() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
}

void oledSplash() {
  oledClear();
  display.setTextSize(2);
  display.println("Port Info");
  display.setTextSize(1);
  display.println(FW_VERSION);
  display.display();
}

void oledLLDP() {
  oledClear();
  display.println("LLDP Info");
  display.println("----------------");
  display.print("Switch: ");
  display.println(sysName);
  display.print("Port:   ");
  display.println(portId);
  display.print("MgmtIP: ");
  display.println(mgmtIp);
  display.print("VLAN:   ");
  display.println(vlanId);
  display.display();
}

void oledDHCP() {
  oledClear();
  display.println("DHCP / IP Info");
  display.println("----------------");
  display.print("MyIP:   ");
  display.println(myIp);
  display.print("MyVLAN: ");
  display.println(myVlan);
  display.print("GW:     ");
  display.println(myGateway);
  display.print("DNS:    ");
  display.println(myDns);
  display.print("DHCP:   ");
  display.println(dhcpOk ? "OK" : "FAIL");
  display.display();
}

void oledInternet() {
  oledClear();
  display.println("Internet Test");
  display.println("----------------");
  display.print("Reachable: ");
  display.println(internetOk ? "YES" : "NO");
  display.print("Latency:   ");
  if (internetOk) {
    display.print(internetLatencyMs);
    display.println(" ms");
  } else {
    display.println("N/A");
  }
  display.display();
}

void oledDiag() {
  linkUp = ETH.linkUp();

  oledClear();
  display.println("Diagnostics");
  display.println("----------------");
  display.print("FW: ");
  display.println(FW_VERSION);
  display.print("Link: ");
  display.println(linkUp ? "UP" : "DOWN");
  display.print("Promisc: ");
  display.println(promiscOn ? "ON" : "OFF");
  display.print("Frames: ");
  display.println(frameCount);
  display.print("LLDP:   ");
  display.println(lldpCount);
  display.print("LastEth: 0x");
  display.println(lastEthertype, HEX);
  display.display();
}

void oledStatusWaitingLLDP() {
  oledClear();
  display.println("Ready");
  display.println("Waiting LLDP...");
  display.display();
}

// ---------------- LLDP Parser ----------------
void parseLLDP(uint8_t *buf, uint32_t len) {
  int idx = 14; // skip Ethernet header
  while (idx + 2 <= (int)len) {
    uint16_t hdr = (buf[idx] << 8) | buf[idx + 1];
    uint8_t type = (hdr >> 9) & 0x7F;
    uint16_t tlvLen = hdr & 0x1FF;
    idx += 2;
    if (tlvLen == 0 || idx + tlvLen > (int)len) break;
    const uint8_t *v = &buf[idx];

    switch (type) {
      case 2: // Port ID
        if (tlvLen > 1) portId = String((char *)(v + 1), tlvLen - 1);
        break;
      case 5: // System Name
        sysName = String((char *)v, tlvLen);
        break;
      case 8: // Mgmt Address
        if (tlvLen >= 7 && v[1] == 1) mgmtIp = ipv4ToStr(&v[2]);
        break;
      case 127: { // Vendor TLV (VLAN)
        if (tlvLen >= 6) {
          uint8_t oui0 = v[0], oui1 = v[1], oui2 = v[2], subtype = v[3];
          bool is8023 = (oui0 == 0x00 && oui1 == 0x12 && oui2 == 0x0F);
          bool isHPE  = (oui0 == 0x00 && oui1 == 0x0A && oui2 == 0x5E);
          bool isAruba= (oui0 == 0x00 && oui1 == 0x1A && oui2 == 0x1E);
          if ((is8023 || isHPE || isAruba) && subtype == 1) {
            vlanId = String((v[4] << 8) | v[5]);
          }
        }
        break;
      }
    }
    idx += tlvLen;
  }

  lldpSeen = true;
  if (uiMode == MODE_LLDP) {
    oledLLDP();
  }
}

// ---------------- Raw Ethernet Callback ----------------
static esp_err_t eth_rx_cb(esp_eth_handle_t h, uint8_t *buf, uint32_t len, void *priv) {
  if (len < 14) return ESP_OK;

  frameCount++;

  uint16_t ethertype = (buf[12] << 8) | buf[13];
  lastEthertype = ethertype;

  // VLAN tag handling
  if (ethertype == 0x8100 && len >= 18) {
    ethertype = (buf[16] << 8) | buf[17];
    lastEthertype = ethertype;
  }

  if (ethertype == 0x88CC) {
    lldpCount++;
    parseLLDP(buf, len);
  }

  return ESP_OK;
}

// ---------------- AP / Web ----------------
void handleJson() {
  String j = "{";

  j += "\"version\":\"" FW_VERSION "\",";

  j += "\"lldp\":{";
  j += "\"systemName\":\"" + sysName + "\",";
  j += "\"portId\":\"" + portId + "\",";
  j += "\"mgmtIp\":\"" + mgmtIp + "\",";
  j += "\"vlanId\":\"" + vlanId + "\"";
  j += "},";

  j += "\"dhcp\":{";
  j += "\"myIp\":\"" + myIp + "\",";
  j += "\"myVlan\":\"" + myVlan + "\",";
  j += "\"gateway\":\"" + myGateway + "\",";
  j += "\"dns\":\"" + myDns + "\",";
  j += "\"ok\":"; j += (dhcpOk ? "true" : "false");
  j += "},";

  j += "\"internet\":{";
  j += "\"reachable\":"; j += (internetOk ? "true" : "false"); j += ",";
  j += "\"latencyMs\":"; j += internetLatencyMs;
  j += "},";

  j += "\"diag\":{";
  j += "\"linkUp\":"; j += (ETH.linkUp() ? "true" : "false"); j += ",";
  j += "\"frames\":"; j += frameCount; j += ",";
  j += "\"lldpCount\":"; j += lldpCount; j += ",";
  j += "\"lastEthertype\":\"0x"; j += String(lastEthertype, HEX); j += "\"";
  j += "}";

  j += "}";

  server.send(200, "application/json", j);
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>PortDiag</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:Arial,Helvetica,sans-serif;background:#111;color:#eee;padding:10px;}h1{font-size:20px;}pre{background:#222;padding:10px;border-radius:4px;}</style>";
  html += "</head><body>";
  html += "<h1>ESP32-POE PortDiag (" FW_VERSION ")</h1>";
  html += "<p>All values are live. Refresh to update.</p>";
  html += "<pre>";

  html += "LLDP\n";
  html += "  Switch: " + sysName + "\n";
  html += "  Port:   " + portId + "\n";
  html += "  MgmtIP: " + mgmtIp + "\n";
  html += "  VLAN:   " + vlanId + "\n\n";

  html += "DHCP / IP\n";
  html += "  MyIP:   " + myIp + "\n";
  html += "  MyVLAN: " + myVlan + "\n";
  html += "  GW:     " + myGateway + "\n";
  html += "  DNS:    " + myDns + "\n";
  html += "  DHCP:   "; html += (dhcpOk ? "OK" : "FAIL"); html += "\n\n";

  html += "Internet\n";
  html += "  Reachable: "; html += (internetOk ? "YES" : "NO"); html += "\n";
  html += "  Latency:   ";
  if (internetOk) {
    html += String(internetLatencyMs) + " ms\n\n";
  } else {
    html += "N/A\n\n";
  }

  html += "Diagnostics\n";
  html += "  Link:    "; html += (ETH.linkUp() ? "UP" : "DOWN"); html += "\n";
  html += "  Frames:  " + String(frameCount) + "\n";
  html += "  LLDP:    " + String(lldpCount) + "\n";
  html += "  LastEth: 0x" + String(lastEthertype, HEX) + "\n";

  html += "</pre>";
  html += "<p>JSON: <a href='/api/status'>/api/status</a></p>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void startAP() {
  // Always-on AP, called once in setup
  WiFi.mode(WIFI_AP);
  WiFi.softAP("PortDiag", "");   // Always "PortDiag"
  server.on("/", handleRoot);
  server.on("/api/status", handleJson);
  server.begin();
}

// ---------------- DHCP / Internet tests ----------------
void runDhcpTest() {
  // On ESP32-ETH, DHCP is handled by ETH.begin() and link up.
  // Here we just read the assigned values.
  IPAddress ip = ETH.localIP();
  IPAddress gw = ETH.gatewayIP();
  IPAddress dns = ETH.dnsIP();

  if (ip == IPAddress(0, 0, 0, 0)) {
    dhcpOk = false;
    myIp = "0.0.0.0";
    myGateway = "0.0.0.0";
    myDns = "0.0.0.0";
  } else {
    dhcpOk = true;
    myIp      = ipToStr(ip);
    myGateway = ipToStr(gw);
    myDns     = ipToStr(dns);
  }

  // VLAN inference from DHCP options would go here; for now:
  myVlan = "Unknown";

  dhcpTestDone = true;
}

void runInternetTest() {
  // Simple DNS + TCP connect test to infer Internet reachability
  internetOk = false;
  internetLatencyMs = 0;

  if (!dhcpOk) {
    internetTestDone = true;
    return;
  }

  IPAddress target;
  uint32_t t0 = millis();
  if (!WiFi.hostByName("www.google.com", target)) {
    internetTestDone = true;
    return;
  }

  WiFiClient client;
  uint32_t t1 = millis();
  if (!client.connect(target, 80)) {
    internetTestDone = true;
    return;
  }
  uint32_t t2 = millis();
  client.stop();

  internetOk = true;
  internetLatencyMs = (t2 - t0); // includes DNS + TCP
  internetTestDone = true;
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  Serial.println("SETUP START");

  esp_bt_controller_disable();

  // OLED / I2C
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  Serial.printf("I2C begin on SDA=%d SCL=%d\n", OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed!");
    while (true) {
      delay(1000);
    }
  }
  Serial.println("OLED init OK");
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.display();

  oledSplash();
  delay(800);

  // Button
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(BUTTON_PIN, buttonISR, FALLING);

  // AP + Web
  startAP();

  // Ethernet
  oled("Init Ethernet...");
  ETH.begin();  // use board defaults for ESP32-POE
  Serial.println("ETH init done");

  // Promiscuous + raw callback
  esp_eth_handle_t h = (esp_eth_handle_t)ETH.handle();
  if (h) {
    bool enable = true;
    esp_eth_ioctl(h, ETH_CMD_S_PROMISCUOUS, &enable);
    promiscOn = true;
    esp_eth_update_input_path(h, eth_rx_cb, nullptr);
    Serial.println("Sniffer active (promisc ON)");
  } else {
    Serial.println("ETH handle is null!");
  }

  oledStatusWaitingLLDP();
}

// ---------------- Loop ----------------
void loop() {
  server.handleClient();

  if (buttonPressed) {
    buttonPressed = false;

    // Advance mode: LLDP -> DHCP -> Internet -> Diag -> LLDP
    switch (uiMode) {
      case MODE_LLDP:
        uiMode = MODE_DHCP;
        dhcpTestDone = false;
        break;
      case MODE_DHCP:
        uiMode = MODE_INTERNET;
        internetTestDone = false;
        break;
      case MODE_INTERNET:
        uiMode = MODE_DIAG;
        break;
      case MODE_DIAG:
      default:
        uiMode = MODE_LLDP;
        break;
    }
  }

  // Run per-mode logic
  switch (uiMode) {
    case MODE_LLDP:
      if (lldpSeen) {
        oledLLDP();
      } else {
        oledStatusWaitingLLDP();
      }
      break;

    case MODE_DHCP:
      if (!dhcpTestDone) {
        runDhcpTest();
      }
      oledDHCP();
      break;

    case MODE_INTERNET:
      if (!internetTestDone) {
        runInternetTest();
      }
      oledInternet();
      break;

    case MODE_DIAG:
      oledDiag();
      break;
  }

  delay(100);
}

void oled(String text) {
  oledClear();
  display.println(text);
  display.display();
  Serial.println("OLED: " + text);
}
