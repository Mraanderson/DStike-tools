#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <Adafruit_NeoPixel.h>
#include <LittleFS.h>

// --- PIN DEFINITIONS ---
#define RGB_PIN 4
#define BUZZER_PIN 5
#define LED_NUM 1

// --- MISSION DEFAULTS (The "Factory Settings") ---
const String DEF_TITLE = "⚠️ SECURITY ALERT ⚠️";
const String DEF_BODY  = "Unauthorized connection detected. System lockdown initiated.";
const String DEF_SSID  = "⚠️ PRIVATE_UPLINK ⚠️";

// --- PROGRAMMABLE VARIABLES (Saved to LittleFS) ---
String pageTitle = DEF_TITLE;
String pageBody  = DEF_BODY;
String apName    = DEF_SSID;
int alertR = 255, alertG = 0, alertB = 0;   // Default Alert: Red
int idleR = 0, idleG = 50, idleB = 0;      // Default Idle: Dim Green
int volume = 10;                           // Volume 1-10 (PWM Duty Cycle)
bool musicEnabled = true;

// --- MUSIC ENGINE DATA ---
unsigned long song_time { 0 };
int note_index { 0 };
bool is_alert_active = false;
bool admin_silence = false; // NEW: Flag to keep alarm off until next disconnect

#define NOTE_C4 262
#define NOTE_D4 294
#define NOTE_E4 330
#define NOTE_F4 349
#define NOTE_G4 392
#define NOTE_A4 440
#define NOTE_B4 494
#define NOTE_C5 523
#define NOTE_D5 587
#define NOTE_E5 659
#define NOTE_F5 698
#define NOTE_G5 784
#define NOTE_A5 880
#define NOTE_B5 988

// Pirates of the Caribbean snippet
int notes[] { NOTE_E4, NOTE_G4, NOTE_A4, NOTE_A4, 0, NOTE_A4, NOTE_B4, NOTE_C5, NOTE_C5, 0, NOTE_C5, NOTE_D5, NOTE_B4, NOTE_B4, 0, NOTE_A4, NOTE_G4, NOTE_A4, 0 }; 
int duration[] { 125, 125, 250, 125, 125, 125, 125, 250, 125, 125, 125, 125, 250, 125, 125, 125, 125, 375, 125 };

// --- OBJECT INITIALIZATION ---
Adafruit_NeoPixel pixels { LED_NUM, RGB_PIN, NEO_GRB + NEO_KHZ800 };
DNSServer dnsServer;
ESP8266WebServer webServer(80);

// --- MEMORY MANAGEMENT (LITTLEFS) ---
void saveSettings() {
  File f = LittleFS.open("/config.txt", "w");
  if (f) {
    f.println(pageBody); f.println(apName);
    f.println(alertR); f.println(alertG); f.println(alertB);
    f.println(idleR); f.println(idleG); f.println(idleB);
    f.println(volume); f.println(musicEnabled);
    f.close();
  }
}

void loadSettings() {
  if (LittleFS.exists("/config.txt")) {
    File f = LittleFS.open("/config.txt", "r");
    if (f) {
      pageBody = f.readStringUntil('\n'); pageBody.trim();
      apName = f.readStringUntil('\n'); apName.trim();
      alertR = f.readStringUntil('\n').toInt(); alertG = f.readStringUntil('\n').toInt(); alertB = f.readStringUntil('\n').toInt();
      idleR = f.readStringUntil('\n').toInt(); idleG = f.readStringUntil('\n').toInt(); idleB = f.readStringUntil('\n').toInt();
      volume = f.readStringUntil('\n').toInt();
      musicEnabled = f.readStringUntil('\n').toInt();
      f.close();
    }
  }
}

// --- AUDIO LOGIC ---
void playSong() {
  if (!musicEnabled) return;
  if (millis() - song_time >= (unsigned long)duration[note_index]) {
    song_time = millis();
    if (notes[note_index] == 0) noTone(BUZZER_PIN);
    else {
      // Volume hack: tone() frequency + delay works for standard speakers.
      tone(BUZZER_PIN, notes[note_index]); 
    }
    note_index++;
    if (note_index >= (int)(sizeof(notes) / sizeof(int))) note_index = 0;
  }
}

// --- THE WEB INTERFACE (HTML/CSS) ---
void handleSettings() {
  // NEW: If "Stop Alarm" is pressed, set active to false AND set silence flag to true
  if (webServer.hasArg("kill")) { 
    is_alert_active = false;
    admin_silence = true;
  }
  
  if (webServer.hasArg("reset")) { 
    pageBody = DEF_BODY; apName = DEF_SSID; 
    alertR = 255; alertG = 0; alertB = 0;
    saveSettings(); 
    ESP.restart(); // Restart to apply new SSID
  }
  
  if (webServer.hasArg("body")) {
    pageBody = webServer.arg("body");
    apName = webServer.arg("ssid");
    alertR = webServer.arg("r").toInt();
    alertG = webServer.arg("g").toInt();
    alertB = webServer.arg("b").toInt();
    volume = webServer.arg("vol").toInt();
    musicEnabled = webServer.hasArg("music");
    saveSettings();
  }

  // Build the "Fancy" UI
  String s = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  s += "<style>body{background:#0a0a0a; color:#00ff41; font-family:monospace; padding:20px;}";
  s += "input, textarea{background:#111; border:1px solid #00ff41; color:#00ff41; width:100%; margin-bottom:10px; padding:5px;}";
  s += ".btn{background:#00ff41; color:#000; border:none; padding:10px; width:100%; font-weight:bold; cursor:pointer;}";
  s += ".kill{background:#ff0000; margin-bottom:20px; color:white;}</style></head><body>";
  
  s += "<h1>⚡ MISSION CONTROL</h1>";
  // The Stop Alarm button now sets the 'kill' argument
  s += "<form action='/settings'><input type='hidden' name='kill' value='1'><input type='submit' class='btn kill' value='STOP ALARM'></form>";
  
  s += "<form action='/settings'>";
  s += "BAIT SSID:<br><input name='ssid' value='" + apName + "'>";
  s += "BODY MESSAGE:<br><textarea name='body' rows='3'>" + pageBody + "</textarea>";
  s += "ALERT COLOR (RGB):<br>";
  s += "<input type='number' name='r' value='"+String(alertR)+"' style='width:30%'>";
  s += "<input type='number' name='g' value='"+String(alertG)+"' style='width:30%'>";
  s += "<input type='number' name='b' value='"+String(alertB)+"' style='width:30%'><br>";
  s += "<input type='checkbox' name='music' " + String(musicEnabled ? "checked" : "") + "> ENABLE MUSIC<br><br>";
  s += "<input type='submit' class='btn' value='UPDATE FIRMWARE'>";
  s += "</form>";
  
  s += "<form action='/settings'><input type='hidden' name='reset' value='1'><input type='submit' style='background:none; border:none; color:#555; text-decoration:underline;' value='Restore Factory Defaults'></form>";
  s += "</body></html>";
  
  webServer.send(200, "text/html", s);
}

void setup() {
  pinMode(BUZZER_PIN, OUTPUT);
  pixels.begin();
  LittleFS.begin();
  loadSettings();
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str());

  // DNS Redirect everything to the stick's IP
  dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));

  // Access the settings via 192.168.4.1/settings
  webServer.on("/settings", handleSettings);

  // The Public "Prank" Page
  webServer.onNotFound([]() {
    // NEW: Check if admin has silenced the device
    if (!admin_silence) {
      is_alert_active = true;
    }
    
    String html = "<html><body style='text-align:center; background:#000; color:red; font-family:sans-serif; padding-top:50px;'>";
    html += "<h1>" + pageTitle + "</h1><p>" + pageBody + "</p></body></html>";
    webServer.send(200, "text/html", html);
  });

  webServer.begin();
}

void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();

  if (is_alert_active) {
    playSong();
    pixels.setPixelColor(0, pixels.Color(alertR, alertG, alertB)); 
    pixels.show();
  } else {
    noTone(BUZZER_PIN);
    pixels.setPixelColor(0, pixels.Color(idleR, idleG, idleB)); 
    pixels.show();
  }

  // Automatic reset when target disconnects
  if (WiFi.softAPgetStationNum() == 0) {
    is_alert_active = false;
    admin_silence = false; // NEW: Reset silence flag so it arms again
    note_index = 0;
  }
}