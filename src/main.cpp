#include <Arduino.h>
#include <ETH.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <WiFi.h>
#include <DNSServer.h>
#include "esp_eth.h"

// Project Metadata
const String projectName = "Port Info";
const String version = "v1.2";

// Hardware Pins
#define UEXT_SDA 13
#define UEXT_SCL 16
#define BUT1_PIN 34 // Olimex ESP32-POE built-in button

// OLED Config
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Web & DNS Server
WebServer server(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);

// Global State
bool eth_connected = false;
bool apActive = false;
int lldpPackets = 0; // DEBUG: Counter to verify sniffer is working
String switchName = "Searching...";
String switchPort = "Searching...";
String switchIP   = "Searching...";
int vlanID        = 0;

// --- WEB DASHBOARD HTML ---
void handleRoot() {
    String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
    html += "body{font-family:sans-serif; background:#1a1a2e; color:#e0e0e0; text-align:center; padding:20px;}";
    html += ".card{background:#16213e; border-radius:12px; padding:20px; margin:10px auto; max-width:400px; border-bottom:4px solid #0f3460; box-shadow: 0 4px 15px rgba(0,0,0,0.3);}";
    html += "h1{color:#e94560; margin-bottom:5px;}";
    html += ".val{color:#4ecca3; font-family:monospace; font-size:1.1em;} hr{border:0; border-top:1px solid #0f3460; margin:15px 0;}";
    html += "</style></head><body>";
    html += "<h1>" + projectName + "</h1><p>" + version + "</p>";
    
    html += "<div class='card'><h3>Switch Discovery</h3><hr>";
    html += "<p>Hostname: <span class='val'>" + switchName + "</span></p>";
    html += "<p>Mgmt IP: <span class='val'>" + switchIP + "</span></p>";
    html += "<p>Port ID: <span class='val'>" + switchPort + "</span></p>";
    html += "<p>VLAN ID: <span class='val'>" + (vlanID == 0 ? "Untagged" : String(vlanID)) + "</span></p></div>";
    
    html += "<div class='card'><h3>Client Status</h3><hr>";
    html += "<p>Local IP: <span class='val'>" + ETH.localIP().toString() + "</span></p>";
    html += "<p>LLDP Frames: <span class='val'>" + String(lldpPackets) + "</span></p>";
    html += "<p>Internet: <span class='val'>" + String(eth_connected ? "YES" : "NO") + "</span></p></div>";
    html += "</body></html>";
    server.send(200, "text/html", html);
}

// --- LLDP PARSER ---
void parseLLDP(uint8_t *data, uint16_t len) {
    lldpPackets++; 
    int pos = 14; 
    while (pos < len) {
        uint16_t tlv = (data[pos] << 8) | data[pos+1];
        uint8_t type = (tlv >> 9);
        uint16_t length = (tlv & 0x01FF);
        pos += 2;
        if (type == 0) break; 
        if (type == 5) { // Hostname
            switchName = "";
            for (int i = 0; i < length; i++) switchName += (char)data[pos + i];
        } 
        else if (type == 2) { // Port
            switchPort = "";
            for (int i = 1; i < length; i++) switchPort += (char)data[pos + i];
        }
        else if (type == 8) { // Mgmt IP
            if (data[pos+1] == 1) { 
                switchIP = String(data[pos+2]) + "." + String(data[pos+3]) + "." + 
                           String(data[pos+4]) + "." + String(data[pos+5]);
            }
        }
        else if (type == 127) { // VLAN
            if (data[pos+3] == 1) { 
                vlanID = (data[pos+4] << 8) | data[pos+5];
            }
        }
        pos += length;
    }
}

esp_err_t eth_recv_cb(esp_eth_handle_t eth_handle, uint8_t *buffer, uint32_t len, void *priv) {
    if (len > 14 && buffer[12] == 0x88 && buffer[13] == 0xcc) {
        parseLLDP(buffer, len);
    }
    free(buffer);
    return ESP_OK;
}

// Function to attach/re-attach sniffer
void attachSniffer() {
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t*) &ETH;
    if (eth_handle != NULL) {
        esp_eth_update_input_path(eth_handle, eth_recv_cb, NULL);
        Serial.println("LLDP Sniffer Active");
    }
}

// --- EVENT HANDLERS ---
void WiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_ETH_GOT_IP:
            eth_connected = true;
            // FIX: Re-attach sniffer here because 802.1X VLAN transitions 
            // often reset the internal driver state
            attachSniffer(); 
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            eth_connected = false;
            switchName = "Searching...";
            vlanID = 0;
            break;
        default: break;
    }
}

void setup() {
    Serial.begin(115200);
    // FIX: GPIO 34 is input-only. No internal pullup available.
    pinMode(BUT1_PIN, INPUT); 

    Wire.begin(UEXT_SDA, UEXT_SCL);
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) for(;;);

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(10, 15);
    display.println(projectName);
    display.setTextSize(1);
    display.setCursor(10, 45);
    display.println("Version " + version);
    display.display();
    delay(2500);

    WiFi.onEvent(WiFiEvent);
    ETH.begin();

    server.on("/", handleRoot);
    server.onNotFound(handleRoot); 

    // Initial attach
    delay(1000); 
    attachSniffer();
}

void loop() {
    // 1. Toggle AP on Button Press
    // Note: If BUT1 is still unresponsive, it may be floating. 
    // We check for a stable LOW state.
    if (digitalRead(BUT1_PIN) == LOW) {
        apActive = !apActive;
        if (apActive) {
            WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
            WiFi.softAP("PortInfo_Probe");
            dnsServer.start(DNS_PORT, "*", apIP);
            server.begin();
            Serial.println("AP Started");
        } else {
            dnsServer.stop();
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_OFF);
            Serial.println("AP Stopped");
        }
        delay(600); // Anti-bounce
    }

    if (apActive) {
        dnsServer.processNextRequest();
        server.handleClient();
    }

    // 2. OLED Update
    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate > 1500) { 
        lastUpdate = millis();
        display.clearDisplay();
        display.setCursor(0,0);
        display.setTextSize(1);
        display.println("SW: " + switchName.substring(0, 15));
        display.println("IP: " + switchIP);
        display.print("Prt: "); display.print(switchPort);
        display.print(" V:"); display.println(vlanID);
        display.println("---------------------");
        
        // Show packet count so we know the sniffer is alive
        display.print("PKTS: "); display.println(lldpPackets);

        if (apActive) {
            display.println("AP: PortInfo_Probe");
        } else {
            display.println("MyIP: " + ETH.localIP().toString());
            display.print(eth_connected ? "WAN: OK" : "WAN: FAIL");
            display.setCursor(85, 55);
            display.print(digitalRead(BUT1_PIN) == LOW ? "PUSH" : "AP?");
        }
        display.display();
    }
}
