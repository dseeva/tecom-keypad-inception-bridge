// Web settings page, JSON API, captive portal (setup mode) and over-the-air firmware upload.
#include <DNSServer.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <time.h>
#include "page.h"
#include "shared.h"

static WebServer server(80);
static DNSServer dns;
static uint32_t rebootAt = 0;
static bool uploadAllowed = false;

static void sendJson(JsonDocument &doc, int code = 200) {
  String s;
  serializeJson(doc, s);
  server.send(code, "application/json", s);
}

static void sendError(const String &e, int code = 400) {
  JsonDocument d;
  d["error"] = e;
  sendJson(d, code);
}

static bool authed() {
  String p;
  { Lock l; p = cfg.web_pass; }
  if (!p.length() || server.authenticate("admin", p.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Keypad Bridge");
  return false;
}

// In setup mode every unknown host name (phones probing for internet) is sent to our page,
// which makes the phone pop up its "sign in to network" screen.
static bool captiveRedirect() {
  if (!gApMode) return false;
  String host = server.hostHeader();
  if (host == WiFi.softAPIP().toString() || host == WiFi.localIP().toString() || host.endsWith(".local")) return false;
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
  return true;
}

static bool body(JsonDocument &doc) {
  DeserializationError e = deserializeJson(doc, server.arg("plain"));
  if (e || !doc.is<JsonObject>()) { sendError("bad request"); return false; }
  return true;
}

static void handleRoot() {
  if (captiveRedirect() || !authed()) return;
  server.send_P(200, "text/html; charset=utf-8", PAGE_HTML);
}

static void handleConfig() {
  if (!authed()) return;
  JsonDocument d;
  configToJson(d);
  {
    Lock l;
    d["has_wifi_pass"] = cfg.wifi_pass.length() > 0;
    d["has_password"] = cfg.password.length() > 0;
    d["has_local_pins"] = cfg.local_pins.length() > 0;
    d["has_web_pass"] = cfg.web_pass.length() > 0;
  }
  JsonArray names = d["led_names"].to<JsonArray>();
  for (int i = 0; i < LED_COUNT; i++) names.add(ledName(i));
  sendJson(d);
}

static void handleSave() {
  if (!authed()) return;
  JsonDocument in;
  if (!body(in)) return;
  String oldHost, oldUser, oldPass;
  { Lock l; oldHost = cfg.inception_host; oldUser = cfg.username; oldPass = cfg.password; }
  bool reboot = false;
  configFromJson(in.as<JsonObjectConst>(), reboot);
  if (!saveConfig()) return sendError("could not save settings", 500);
  {
    Lock l;
    if (oldHost != cfg.inception_host || oldUser != cfg.username || oldPass != cfg.password) inceptionReloadNames();
  }
  logf("settings saved%s", reboot ? " - restarting" : "");
  kpShow("", 0);
  JsonDocument d;
  d["ok"] = true;
  d["reboot"] = reboot;
  sendJson(d);
  if (reboot) rebootAt = millis() + 1500;
}

static void handleTest() {
  if (!authed()) return;
  JsonDocument in;
  if (!body(in)) return;
  String host = in["inception_host"] | "", user = in["username"] | "", pass = in["password"] | "";
  if (!pass.length()) { Lock l; pass = cfg.password; }
  String msg;
  bool ok = inceptionTest(host, user, pass, msg);
  JsonDocument d;
  d["ok"] = ok;
  d["msg"] = msg;
  sendJson(d);
}

static void handleStatus() {
  if (!authed()) return;
  JsonDocument d;
  String scr = kpScreen();
  scr.replace("|", "\n");
  d["screen"] = scr;
  JsonObject k = d["kp"].to<JsonObject>();
  kpStats(k);
  d["keypad"] = String((uint32_t)k["polls"]) + " polls, " + String((uint32_t)k["replies"]) + " replies";
  d.remove("kp");
  {
    Lock l;
    d["inception"] = inc.error.length() ? inc.error : String("connected");
    d["hostname"] = cfg.hostname;
  }
  d["setup"] = gApMode && !WiFi.isConnected();
  if (WiFi.isConnected()) {
    d["wifi"] = WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)";
    d["address"] = "http://" + d["hostname"].as<String>() + ".local  (" + WiFi.localIP().toString() + ")";
  } else {
    d["wifi"] = gApMode ? "not connected - setup network \"" + gApName + "\" is on" : String("connecting...");
    d["address"] = gApMode ? "http://" + WiFi.softAPIP().toString() : String("-");
  }
  time_t now = time(nullptr);
  if (now > 1600000000) {
    char b[32];
    struct tm t;
    localtime_r(&now, &t);
    strftime(b, sizeof b, "%d/%m/%Y %H:%M:%S", &t);
    d["time"] = b;
  } else {
    d["time"] = "not set yet (needs internet)";
  }
  d["version"] = FW_VERSION;
  d["heap"] = String(ESP.getFreeHeap() / 1024) + " KB";
  logLines(d["log"].to<JsonArray>(), 40);
  sendJson(d);
}

static void handleEntities() {
  if (!authed()) return;
  JsonDocument d;
  Lock l;
  d["loaded"] = inc.namesLoaded;
  auto add = [&](const char *key, const std::map<String, String> &m) {
    JsonArray a = d[key].to<JsonArray>();
    for (auto &kv : m) {
      JsonObject o = a.add<JsonObject>();
      o["ID"] = kv.first;
      o["Name"] = kv.second;
    }
  };
  add("areas", inc.areaNames);
  add("inputs", inc.inputNames);
  add("doors", inc.doorNames);
  sendJson(d);
}

static void handleScan() {
  if (!authed()) return;
  int n = WiFi.scanNetworks();
  JsonDocument d;
  JsonArray a = d["nets"].to<JsonArray>();
  std::set<String> seen;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || seen.count(s)) continue;
    seen.insert(s);
    JsonObject o = a.add<JsonObject>();
    o["ssid"] = s;
    o["rssi"] = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  sendJson(d);
}

static void handleLedTest() {
  if (!authed()) return;
  JsonDocument in;
  if (!body(in)) return;
  kpLedTest(in["action"] | "", in["bytes"] | "");
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleReboot() {
  if (!authed()) return;
  server.send(200, "application/json", "{\"ok\":true}");
  rebootAt = millis() + 1000;
}

static void handleUpdateDone() {
  if (!authed()) return;
  bool ok = uploadAllowed && !Update.hasError() && Update.isFinished();
  server.send(ok ? 200 : 500, "text/plain",
              ok ? "Firmware installed - restarting" : String("Update failed: ") + Update.errorString());
  if (ok) rebootAt = millis() + 1000;
}

static void handleUpload() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    String p;
    { Lock l; p = cfg.web_pass; }
    uploadAllowed = !p.length() || server.authenticate("admin", p.c_str());
    if (uploadAllowed) { logf("firmware upload started"); Update.begin(UPDATE_SIZE_UNKNOWN); }
  } else if (!uploadAllowed) {
    return;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    Update.write(u.buf, u.currentSize);
  } else if (u.status == UPLOAD_FILE_END) {
    if (Update.end(true)) logf("firmware upload complete (%u bytes)", (unsigned)u.totalSize);
    else logf("firmware upload failed: %s", Update.errorString());
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
  }
}

void webStartPortal() { dns.start(53, "*", WiFi.softAPIP()); }
void webStopPortal() { dns.stop(); }

void webBegin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/config", HTTP_GET, handleConfig);
  server.on("/api/config", HTTP_POST, handleSave);
  server.on("/api/test", HTTP_POST, handleTest);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/entities", HTTP_GET, handleEntities);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/ledtest", HTTP_POST, handleLedTest);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpload);
  server.onNotFound([]() {
    if (captiveRedirect()) return;
    server.send(404, "text/plain", "not found");
  });
  server.begin();
}

void webLoop() {
  if (gApMode) dns.processNextRequest();
  server.handleClient();
  if (rebootAt && (int32_t)(millis() - rebootAt) > 0) ESP.restart();
}
