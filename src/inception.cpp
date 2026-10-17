// Inner Range Inception REST API: login, state polling, area arm/disarm, door control, PIN check.
// Runs in its own task so a slow API call never stalls the keypad polling.
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "shared.h"

struct Action {
  bool arm;
  char pin[16];
  char slot[64];
};

static QueueHandle_t actionQ;
static String sess;                 // LoginSessId cookie (inception task only)
static volatile bool reloadNames = true, dropSession = false;
static uint32_t namesAt = 0;

static String apiRoot(String h) {
  h.trim();
  while (h.endsWith("/")) h.remove(h.length() - 1);
  if (!h.startsWith("http://") && !h.startsWith("https://")) h = "http://" + h;
  return h + "/api/v1";
}

// One HTTP request. JSON responses are parsed into `out` (optionally through a filter to save memory).
static int rawCall(const String &root, const char *method, const String &path, const String &body,
                   const String &session, JsonDocument &out, JsonDocument *filter, String &err) {
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  http.useHTTP10(true);             // no chunked encoding, so the JSON can be parsed straight from the stream
  http.setConnectTimeout(5000);
  http.setTimeout(15000);
  String url = root + path;
  bool begun;
  if (url.startsWith("https://")) { secure.setInsecure(); begun = http.begin(secure, url); }
  else begun = http.begin(plain, url);
  if (!begun) { err = "bad Inception address"; return -1; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  if (session.length()) http.addHeader("Cookie", "LoginSessId=" + session);
  int code = http.sendRequest(method, body);
  out.clear();
  if (code <= 0) {
    err = "cannot reach Inception (" + HTTPClient::errorToString(code) + ")";
  } else if (code >= 200 && code < 300) {
    DeserializationError e = filter ? deserializeJson(out, http.getStream(), DeserializationOption::Filter(*filter))
                                    : deserializeJson(out, http.getStream());
    if (e && e != DeserializationError::EmptyInput) { err = String("bad reply from Inception: ") + e.c_str(); code = -100; }
  } else {
    err = "Inception replied HTTP " + String(code);
  }
  http.end();
  return code;
}

static bool login(const String &root, const String &user, const String &pass, String &session, String &err) {
  JsonDocument req, out;
  req["Username"] = user;
  req["Password"] = pass;
  String body;
  serializeJson(req, body);
  int code = rawCall(root, "POST", "/authentication/login", body, "", out, nullptr, err);
  if (code != 200) return false;
  if (String((const char *)(out["Response"]["Result"] | "")) != "Success" || !out["UserID"].is<const char *>()) {
    err = "Inception login failed - check the API username and password";
    return false;
  }
  session = out["UserID"].as<String>();
  return true;
}

// Authenticated call with the saved settings; logs in again once if the session has expired.
static int call(const char *method, const String &path, const String &body, JsonDocument &out,
                JsonDocument *filter, String &err) {
  String host, user, pass;
  {
    Lock l;
    host = cfg.inception_host; user = cfg.username; pass = cfg.password;
  }
  if (!host.length() || !user.length()) { err = "Inception not set up yet"; return -1; }
  String root = apiRoot(host);
  if (!sess.length() && !login(root, user, pass, sess, err)) return -1;
  int code = rawCall(root, method, path, body, sess, out, filter, err);
  if (code == 401) {
    sess = "";
    if (!login(root, user, pass, sess, err)) return -1;
    code = rawCall(root, method, path, body, sess, out, filter, err);
  }
  return code;
}

static bool states(const char *type, std::map<String, int> &out, String &err) {
  String body = String("[{\"ID\":\"req\",\"RequestType\":\"MonitorEntityStates\",\"InputData\":{\"stateType\":\"") +
                type + "\",\"timeSinceUpdate\":\"0\"}}]";
  JsonDocument doc;
  if (call("POST", "/monitor-updates", body, doc, nullptr, err) != 200) return false;
  JsonVariant res = doc.is<JsonArray>() ? doc[0]["Result"].as<JsonVariant>() : doc["Result"].as<JsonVariant>();
  out.clear();
  for (JsonObject s : res["stateData"].as<JsonArray>()) {
    int v = s["PublicState"].is<int>() ? s["PublicState"].as<int>() : (s["stateValue"] | 0);
    out[s["ID"].as<String>()] = v;
  }
  return true;
}

static bool names(const char *path, std::map<String, String> &out, std::set<String> *custom, String &err) {
  JsonDocument filter, doc;
  filter[0]["ID"] = true;
  filter[0]["Name"] = true;
  if (custom) filter[0]["IsCustomInput"] = true;
  if (call("GET", path, "", doc, &filter, err) != 200) return false;
  out.clear();
  if (custom) custom->clear();
  for (JsonObject o : doc.as<JsonArray>()) {
    out[o["ID"].as<String>()] = o["Name"].as<String>();
    if (custom && (o["IsCustomInput"] | false)) custom->insert(o["ID"].as<String>());
  }
  return true;
}

static void pollOnce() {
  if (dropSession) { sess = ""; dropSession = false; }
  String err;
  std::map<String, int> ast, ist;
  std::map<String, String> an, in, dn;
  std::set<String> cust;
  bool fresh = reloadNames || !namesAt || millis() - namesAt > 300000;
  if (fresh) {
    if (!names("/control/area", an, nullptr, err) || !names("/control/input", in, &cust, err)) goto fail;
    if (!names("/control/door", dn, nullptr, err)) { logf("door list unavailable: %s", err.c_str()); dn.clear(); err = ""; }
  }
  if (!states("AreaState", ast, err) || !states("InputState", ist, err)) goto fail;
  {
    Lock l;
    if (fresh) {
      inc.areaNames = an; inc.inputNames = in; inc.doorNames = dn; inc.custom = cust;
      inc.namesLoaded = true; namesAt = millis(); reloadNames = false;
    }
    inc.areaStates = ast;
    inc.inputStates = ist;
    std::set<String> faultIds;   // inputs feeding the COMMS / RAS / DGP / BATTERY LEDs
    for (int i = 0; i < 4; i++) {
      auto it = cfg.leds.find(SYS_LEDS[i]);
      if (it == cfg.leds.end()) continue;
      for (auto &s : it->second.sources) if (s.startsWith("input:")) faultIds.insert(s.substring(6));
    }
    inc.openInputs.clear();
    inc.faults.clear();
    for (auto &kv : ist) {
      if (!(kv.second & I_ACTIVE)) continue;
      auto nm = inc.inputNames.find(kv.first);
      String name = nm != inc.inputNames.end() ? nm->second : kv.first;
      if (cfg.show_custom || !inc.custom.count(kv.first)) inc.openInputs.push_back(name);
      if (faultIds.count(kv.first)) inc.faults.push_back(name);
    }
    if (inc.error.length()) logf("Inception connected");
    inc.error = "";
  }
  return;
fail:
  sess = "";
  {
    Lock l;
    if (inc.error != err) logf("Inception: %s", err.c_str());
    inc.error = err;
  }
}

static bool pinOk(const String &pin) {
  String mode, pins;
  {
    Lock l;
    mode = cfg.pin_mode; pins = cfg.local_pins;
  }
  if (!pin.length()) return false;
  if (mode == "inception") {
    // GET /config/user/by-pin/{pin}: 404 = no user has this PIN
    for (char c : pin) if (!isdigit((unsigned char)c)) return false;
    JsonDocument filter, doc;
    filter["ID"] = true; filter["Name"] = true; filter["UserCancelled"] = true;
    String err;
    int code = call("GET", "/config/user/by-pin/" + pin, "", doc, &filter, err);
    if (code == 404) return false;
    if (code != 200) { logf("PIN lookup failed: %s", err.c_str()); return false; }
    if (!doc["ID"].is<const char *>() || (doc["UserCancelled"] | false)) return false;
    logf("PIN accepted for Inception user %s", (const char *)(doc["Name"] | "?"));
    return true;
  }
  int start = 0;
  while (start <= (int)pins.length()) {
    int comma = pins.indexOf(',', start);
    if (comma < 0) comma = pins.length();
    String p = pins.substring(start, comma);
    p.trim();
    if (p.length() && p == pin) return true;
    start = comma + 1;
  }
  return false;
}

static void doAction(const Action &a) {
  String slot = a.slot;
  bool door = slot.startsWith("door:");
  String id = slot.substring(5), name, doorType;
  {
    Lock l;
    auto &m = door ? inc.doorNames : inc.areaNames;
    name = m.count(id) ? m[id] : (door ? "Door" : "Area");
    doorType = cfg.door_action;
  }
  if (!pinOk(a.pin)) {
    logf("invalid code entered");
    kpShow("Invalid code|Code: ", 3);
    return;
  }
  JsonDocument req, out;
  String path;
  if (door) {
    req["Type"] = "ControlDoor";
    req["DoorControlType"] = a.arm ? doorType : String("Lock");
    path = "/control/door/" + id + "/activity";
  } else {
    req["Type"] = "ControlArea";
    req["AreaControlType"] = a.arm ? "Arm" : "Disarm";
    path = "/control/area/" + id + "/activity";
  }
  String body, err;
  serializeJson(req, body);
  int code = call("POST", path, body, out, nullptr, err);
  String result = out["Response"]["Result"] | "";
  bool ok = code == 200 && result == "Success";
  const char *what = door ? (a.arm ? (doorType == "Unlock" ? "unlocked" : "opened") : "locked") : (a.arm ? "armed" : "disarmed");
  logf("%s %s: %s", name.c_str(), what, ok ? "ok" : (err.length() ? err.c_str() : result.c_str()));
  if (ok) kpShow(name + " " + what + "|Area # or code: ", 3);
  else kpShow(String("Failed|") + (err.length() ? err : result).substring(0, LCD_W), 4);
}

static void task(void *) {
  for (;;) {
    Action a;
    if (xQueueReceive(actionQ, &a, 0) == pdTRUE) {
      if (WiFi.isConnected()) doAction(a);
      else kpShow("No Wi-Fi|Code: ", 3);
      kpDone();
      continue;
    }
    if (WiFi.isConnected()) pollOnce();
    else { Lock l; inc.error = "no Wi-Fi connection"; }
    // wait ~2 s between polls, but wake straight away for a keypad action
    if (xQueuePeek(actionQ, &a, pdMS_TO_TICKS(2000)) == pdTRUE) continue;
  }
}

bool inceptionQueue(bool arm, const String &pin, const String &slot) {
  Action a;
  a.arm = arm;
  strlcpy(a.pin, pin.c_str(), sizeof a.pin);
  strlcpy(a.slot, slot.c_str(), sizeof a.slot);
  return xQueueSend(actionQ, &a, 0) == pdTRUE;
}

void inceptionReloadNames() { reloadNames = true; dropSession = true; }

bool inceptionTest(const String &host, const String &user, const String &pass, String &msg) {
  if (!WiFi.isConnected()) { msg = "the bridge is not on your Wi-Fi yet"; return false; }
  if (!host.length() || !user.length()) { msg = "enter the address and username first"; return false; }
  String root = apiRoot(host), s, err;
  if (!login(root, user, pass, s, err)) { msg = err; return false; }
  JsonDocument filter, doc;
  filter[0]["ID"] = true;
  int code = rawCall(root, "GET", "/control/area", "", s, doc, &filter, err);
  if (code != 200) { msg = "logged in, but listing areas failed: " + err; return false; }
  msg = "Connected - " + String(doc.as<JsonArray>().size()) + " areas visible. Save to use these settings.";
  return true;
}

void inceptionBegin() {
  actionQ = xQueueCreate(2, sizeof(Action));
  xTaskCreatePinnedToCore(task, "inception", 16384, nullptr, 1, nullptr, 0);
}
