// Tecom TS0804 keypad <-> Inner Range Inception bridge (ESP32).
//
// The ESP32 acts as the keypad's panel on its RS485 bus and talks to Inception over Wi-Fi.
// First boot (or no saved Wi-Fi): it opens the open Wi-Fi network "KeypadBridge-XXXX" with a
// captive portal; after setup it joins the home network and serves the settings page at
// http://<hostname>.local.  Hold BOOT for 5 s to forget the Wi-Fi settings.
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include "shared.h"

bool gApMode = false;
String gApName;

static bool wasConnected = false, mdnsUp = false;
static uint32_t wifiSince = 0, lastRetry = 0, connectedAt = 0, bootDown = 0;

static void beginStation() {
  String ssid, pass;
  { Lock l; ssid = cfg.wifi_ssid; pass = cfg.wifi_pass; }
  WiFi.begin(ssid.c_str(), pass.length() ? pass.c_str() : nullptr);
  lastRetry = millis();
}

static void startAP() {
  if (gApMode) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(gApName.c_str());
  delay(100);
  webStartPortal();
  gApMode = true;
  logf("setup network \"%s\" is on - join it and open http://%s", gApName.c_str(), WiFi.softAPIP().toString().c_str());
}

static void stopAP() {
  webStopPortal();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  gApMode = false;
  logf("setup network turned off");
}

static void wifiLoop() {
  bool up = WiFi.isConnected();
  bool haveSsid;
  String host;
  { Lock l; haveSsid = cfg.wifi_ssid.length() > 0; host = cfg.hostname; }
  if (up && !wasConnected) {
    wasConnected = true;
    connectedAt = millis();
    String ip = WiFi.localIP().toString();
    logf("Wi-Fi connected to %s, address %s", WiFi.SSID().c_str(), ip.c_str());
    if (!mdnsUp && MDNS.begin(host.c_str())) {
      MDNS.addService("http", "tcp", 80);
      mdnsUp = true;
      logf("settings page: http://%s.local", host.c_str());
    }
    kpShow("IP " + ip + "|" + host + ".local", 10);
  } else if (!up && wasConnected) {
    wasConnected = false;
    wifiSince = millis();
    logf("Wi-Fi connection lost");
  }
  if (!haveSsid || up) {
    // joined the home network: drop the setup network once nobody is using it
    if (gApMode && up && WiFi.softAPgetStationNum() == 0 && millis() - connectedAt > 30000) stopAP();
    return;
  }
  // a saved network that cannot be reached for 30 s: open the setup network as well
  if (!gApMode && millis() - wifiSince > 30000) startAP();
  // keep retrying the home network, but not while a phone is on the setup page
  if (gApMode && WiFi.softAPgetStationNum() == 0 && millis() - lastRetry > 30000) beginStation();
}

static void bootButtonLoop() {
  if (digitalRead(BOOT_BUTTON_PIN) == HIGH) { bootDown = 0; return; }
  if (!bootDown) { bootDown = millis(); return; }
  if (millis() - bootDown < 5000) return;
  logf("BOOT held - forgetting Wi-Fi settings");
  { Lock l; cfg.wifi_ssid = ""; cfg.wifi_pass = ""; }
  saveConfig();
  kpShow("Wi-Fi settings cleared|restarting...", 5);
  delay(1500);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  delay(100);
  gLock = xSemaphoreCreateRecursiveMutex();
  logf("Keypad bridge %s starting", FW_VERSION);
  if (!LittleFS.begin(true)) logf("flash file system failed to start");
  logf(loadConfig() ? "settings loaded" : "no saved settings - using defaults");

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[32];
  snprintf(name, sizeof name, "KeypadBridge-%02X%02X", mac[4], mac[5]);
  gApName = name;

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  keypadBegin();

  WiFi.persistent(false);              // Wi-Fi details live in our own settings file
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(cfg.hostname.c_str());
  if (cfg.wifi_ssid.length()) {
    WiFi.mode(WIFI_STA);
    beginStation();
    logf("joining Wi-Fi \"%s\" as %s", cfg.wifi_ssid.c_str(), cfg.hostname.c_str());
  } else {
    startAP();
  }
  wifiSince = millis();
  configTzTime(cfg.tz.c_str(), cfg.ntp.c_str());
  webBegin();
  inceptionBegin();
}

void loop() {
  webLoop();
  wifiLoop();
  bootButtonLoop();
  delay(2);
}
