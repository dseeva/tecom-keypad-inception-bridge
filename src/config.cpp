// Settings: kept as JSON in the ESP32's own flash (LittleFS /config.json), never in the source.
#include <LittleFS.h>
#include "shared.h"

Config cfg;
IncState inc;
SemaphoreHandle_t gLock;

static const char *CFG_FILE = "/config.json";
const char *const SYS_LEDS[8] = {"COMMS", "RAS", "DGP", "BATTERY", "ACCESS", "24HR", "SECURE", "TAMPER"};

String ledName(int i) { return i < 8 ? String(SYS_LEDS[i]) : "AREA" + String(i - 7); }

static bool validLed(const String &n) {
  for (int i = 0; i < LED_COUNT; i++) if (ledName(i) == n) return true;
  return false;
}

String slotId(int n, const char *kind) {
  if (n < 1 || n > KEYPAD_SLOTS) return "";
  String p = String(kind) + ":";
  return cfg.slots[n].startsWith(p) ? cfg.slots[n].substring(p.length()) : "";
}

String primaryArea() { return slotId(1, "area"); }

static String cleanHostname(String h) {
  h.toLowerCase();
  String out;
  for (char c : h) if (isalnum((unsigned char)c) || c == '-') out += c;
  while (out.startsWith("-")) out.remove(0, 1);
  if (out.length() > 32) out = out.substring(0, 32);
  return out.length() ? out : "keypad-bridge";
}

static void setStr(JsonObjectConst in, const char *key, String &dst) {
  if (in[key].is<const char *>()) dst = in[key].as<const char *>();
}

// Secrets are only replaced when a non-empty value is sent ("leave blank to keep").
static void setSecret(JsonObjectConst in, const char *key, String &dst) {
  if (in[key].is<const char *>() && strlen(in[key].as<const char *>())) dst = in[key].as<const char *>();
}

bool configFromJson(JsonObjectConst in, bool &needReboot) {
  Lock l;
  String oldSsid = cfg.wifi_ssid, oldPass = cfg.wifi_pass, oldHost = cfg.hostname;
  setStr(in, "wifi_ssid", cfg.wifi_ssid);
  setSecret(in, "wifi_pass", cfg.wifi_pass);
  if (in["hostname"].is<const char *>()) cfg.hostname = cleanHostname(in["hostname"].as<String>());
  setStr(in, "tz", cfg.tz);
  setStr(in, "ntp", cfg.ntp);
  setSecret(in, "web_pass", cfg.web_pass);
  if (in["clear_web_pass"] | false) cfg.web_pass = "";
  setStr(in, "inception_host", cfg.inception_host);
  setStr(in, "username", cfg.username);
  setSecret(in, "password", cfg.password);
  setStr(in, "pin_mode", cfg.pin_mode);
  setSecret(in, "local_pins", cfg.local_pins);
  setStr(in, "key_mode", cfg.key_mode);
  setStr(in, "door_action", cfg.door_action);
  setStr(in, "idle_message", cfg.idle_message);
  setStr(in, "armed_message", cfg.armed_message);
  setStr(in, "fault_message", cfg.fault_message);
  if (in["show_custom"].is<bool>()) cfg.show_custom = in["show_custom"];
  if (in["show_custom_inputs"].is<bool>()) cfg.show_custom = in["show_custom_inputs"];   // PC bridge name
  if (cfg.pin_mode != "inception") cfg.pin_mode = "local";
  if (cfg.key_mode != "toggle") cfg.key_mode = "labels";
  if (cfg.door_action != "Unlock") cfg.door_action = "Open";

  // keypad slots: {"1": "area:<id>", "3": "door:<id>"}; the PC bridge's "keypad_areas" holds bare area ids
  JsonObjectConst ka = in["keypad_areas"];
  JsonObjectConst sl = in["slots"].is<JsonObjectConst>() ? in["slots"].as<JsonObjectConst>() : ka;
  if (!sl.isNull()) {
    for (int n = 1; n <= KEYPAD_SLOTS; n++) cfg.slots[n] = "";
    for (JsonPairConst p : sl) {
      int n = atoi(p.key().c_str());
      String v = p.value().as<String>();
      if (n < 1 || n > KEYPAD_SLOTS || !v.length()) continue;
      if (!v.startsWith("area:") && !v.startsWith("door:")) v = "area:" + v;
      cfg.slots[n] = v;
    }
  }
  // LEDs: {"AREA1": {"sources": ["area:<id>"], "mode": "on"|"flash"}} (PC bridge calls it "led_cfg")
  JsonObjectConst lc = in["leds"].is<JsonObjectConst>() ? in["leds"].as<JsonObjectConst>() : in["led_cfg"].as<JsonObjectConst>();
  if (!lc.isNull()) {
    cfg.leds.clear();
    for (JsonPairConst p : lc) {
      String name = p.key().c_str();
      if (!validLed(name)) continue;
      LedCfg c;
      for (JsonVariantConst s : p.value()["sources"].as<JsonArrayConst>()) {
        String src = s.as<String>();
        if (src.startsWith("area:") || src.startsWith("input:")) c.sources.push_back(src);
      }
      c.flash = String((const char *)(p.value()["mode"] | "on")) == "flash";
      if (!c.sources.empty()) cfg.leds[name] = c;
    }
  }
  needReboot = cfg.wifi_ssid != oldSsid || cfg.wifi_pass != oldPass || cfg.hostname != oldHost;
  return true;
}

void configToJson(JsonDocument &doc) {
  Lock l;
  doc["wifi_ssid"] = cfg.wifi_ssid;
  doc["hostname"] = cfg.hostname;
  doc["tz"] = cfg.tz;
  doc["ntp"] = cfg.ntp;
  doc["inception_host"] = cfg.inception_host;
  doc["username"] = cfg.username;
  doc["pin_mode"] = cfg.pin_mode;
  doc["key_mode"] = cfg.key_mode;
  doc["door_action"] = cfg.door_action;
  doc["idle_message"] = cfg.idle_message;
  doc["armed_message"] = cfg.armed_message;
  doc["fault_message"] = cfg.fault_message;
  doc["show_custom"] = cfg.show_custom;
  JsonObject sl = doc["slots"].to<JsonObject>();
  for (int n = 1; n <= KEYPAD_SLOTS; n++) if (cfg.slots[n].length()) sl[String(n)] = cfg.slots[n];
  JsonObject lc = doc["leds"].to<JsonObject>();
  for (auto &kv : cfg.leds) {
    JsonObject o = lc[kv.first].to<JsonObject>();
    JsonArray a = o["sources"].to<JsonArray>();
    for (auto &s : kv.second.sources) a.add(s);
    o["mode"] = kv.second.flash ? "flash" : "on";
  }
}

bool saveConfig() {
  JsonDocument doc;
  configToJson(doc);
  {
    Lock l;
    doc["wifi_pass"] = cfg.wifi_pass;
    doc["password"] = cfg.password;
    doc["local_pins"] = cfg.local_pins;
    doc["web_pass"] = cfg.web_pass;
  }
  File f = LittleFS.open("/config.tmp", "w");
  if (!f) { logf("cannot write settings"); return false; }
  serializeJson(doc, f);
  f.close();
  LittleFS.remove(CFG_FILE);
  LittleFS.rename("/config.tmp", CFG_FILE);
  return true;
}

bool loadConfig() {
  File f = LittleFS.open(CFG_FILE, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) { logf("settings file unreadable (%s), using defaults", e.c_str()); return false; }
  bool reboot;
  configFromJson(doc.as<JsonObjectConst>(), reboot);
  return true;
}
