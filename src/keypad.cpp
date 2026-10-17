// Tecom TS0804 keypad driven as if this bridge were its Challenger panel.
//
// Bus: 4800 8N1. Packets "5e DEST CMD [LEN DATA] CHK", CHK = ~XOR of every byte after 5e. Keypad address 0x10.
//   poll         5e 90 00 6f           -> keypad 00 (no key) or 07 N k1..kN CHK (queued keys)
//   key ack      5e 10 10 ff           (unacked, the keypad keeps resending and stops after 10 keys)
//   screen       5e 10 01 LEN 0b <ASCII, 0a = new line> CHK
//   LEDs         5e 10 11 07 d0..d6 CHK (2 bits per LED: 01 steady, 10 flashing)
//   cycle tail   5e a1 00 80 de        (the real panel sends it every ~160 ms cycle)
#include <WiFi.h>
#include <deque>
#include <time.h>
#include "driver/uart.h"
#include "shared.h"

static HardwareSerial Bus(2);
static const uint32_t CYCLE_MS = 160;

// keypad-task-only state
static std::deque<std::vector<uint8_t>> todo;
static uint8_t lastSent[80];
static int lastSentLen = 0;
static uint8_t ledSent[7];
static bool ledValid = false;
static uint32_t lastLed = 0, lastKey = 0, started = 0;
static String entry, sel, shown;

// shared with the web and Inception tasks (guarded by gLock)
static String msg;
static uint32_t msgUntil = 0;
static bool shownValid = false, busy = false;
static uint32_t polls = 0, replies = 0;
static bool ledOverride = false;
static uint8_t ledOv[7];
static int32_t testStart = -1;    // LED test start time (ms), -1 = not running
static String testText;

static const char *keyName(uint8_t k) {
  switch (k) {
    case 0x3a: return "MENU";  case 0x3b: return "ENTER"; case 0x3c: return "NEXT";
    case 0x3d: return "CLEAR"; case 0x3e: return "ON";    case 0x3f: return "OFF";
  }
  return "?";
}

static String ascii(const String &s) {
  String o;
  for (char c : s) o += ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7f) || c == '|' ? c : '?';
  return o;
}

// ---------------------------------------------------------------- bus I/O
static void sendBody(const uint8_t *body, int n) {
  uint8_t pkt[80];
  int len = 0;
  uint8_t x = 0;
  pkt[len++] = 0x5e;
  for (int i = 0; i < n && len < 79; i++) { pkt[len++] = body[i]; x ^= body[i]; }
  pkt[len++] = (uint8_t)~x;
  int late = 0;
  while (Bus.available()) { Bus.read(); late++; }
  if (late) logf("%d late bytes from keypad", late);
  if (BUS_DE_PIN >= 0) digitalWrite(BUS_DE_PIN, HIGH);
  Bus.write(pkt, len);
  Bus.flush();                       // returns once the last stop bit is out
  if (BUS_DE_PIN >= 0) digitalWrite(BUS_DE_PIN, LOW);
  memcpy(lastSent, pkt, len);
  lastSentLen = len;
}

static void sendBody(std::initializer_list<uint8_t> b) {
  std::vector<uint8_t> v(b);
  sendBody(v.data(), v.size());
}

// Collect the keypad's reply: wait up to firstMs for the first byte, then until gapMs of silence.
// If the RS485 module hears its own transmission, that echo is stripped off the front.
static int readReply(uint8_t *buf, int max, uint32_t firstMs = 60, uint32_t gapMs = 15) {
  int n = 0;
  uint32_t t0 = millis(), last = 0;
  for (;;) {
    if (Bus.available()) {
      int c = Bus.read();
      if (n < max) buf[n++] = c;
      last = millis();
      continue;
    }
    uint32_t now = millis();
    if ((n == 0 && now - t0 > firstMs) || (n > 0 && now - last > gapMs)) break;
    delay(1);
  }
  if (n >= lastSentLen && lastSentLen && memcmp(buf, lastSent, lastSentLen) == 0) {
    memmove(buf, buf + lastSentLen, n - lastSentLen);
    n -= lastSentLen;
  }
  return n;
}

// ---------------------------------------------------------------- screen text
static int areaState(const String &id) {
  auto it = inc.areaStates.find(id);
  return it == inc.areaStates.end() ? 0 : it->second;
}

static String nameOf(const std::map<String, String> &m, const String &id, const char *dflt) {
  auto it = m.find(id);
  return it == m.end() ? String(dflt) : it->second;
}

static String fmtTime(const char *f) {
  time_t now = time(nullptr);
  if (now < 1600000000) return f[1] == 'd' ? "--/--/--" : "--:--";
  struct tm t;
  localtime_r(&now, &t);
  char b[16];
  strftime(b, sizeof b, f, &t);
  return b;
}

static String areasText() {
  String s;
  for (int n = 1; n <= KEYPAD_SLOTS; n++) {
    String a = slotId(n, "area");
    if (!a.length()) continue;
    int st = areaState(a);
    if (s.length()) s += "  ";
    s += String(n) + " " + nameOf(inc.areaNames, a, "?") + " " + (st & A_ALARM ? "ALARM" : st & A_ARMED ? "ARMED" : "off");
  }
  return s;
}

static String fillLine(const String &tmpl, const String &unsealed) {
  String pa = primaryArea();
  int st = areaState(pa);
  String state = !inc.areaStates.count(pa) ? "unknown" : st & A_ALARM ? "ALARM" : st & A_ARMED ? "armed" : "disarmed";
  String faults;
  for (auto &f : inc.faults) faults += (faults.length() ? ", " : "") + f;
  String s = tmpl;
  s.replace("{date}", fmtTime("%d/%m/%y"));
  s.replace("{time}", fmtTime("%H:%M"));
  s.replace("{area}", nameOf(inc.areaNames, pa, "Area"));
  s.replace("{state}", state);
  s.replace("{unsealed}", unsealed);
  s.replace("{areas}", areasText());
  s.replace("{faults}", faults.length() ? faults : "none");
  return s;
}

static String fillTmplLine(const String &tmpl) {
  auto &names = inc.openInputs;
  String all;
  for (auto &n : names) all += (all.length() ? ", " : "") + n;
  String line = fillLine(tmpl, names.empty() ? "All sealed" : all);
  if (tmpl.indexOf("{unsealed}") >= 0 && line.length() > LCD_W && names.size() > 1) {
    size_t i = (millis() / 2500) % names.size();     // show one open input at a time
    line = fillLine(tmpl, names[i] + " (" + String(i + 1) + "/" + String(names.size()) + ")");
  }
  return line.substring(0, LCD_W);
}

static String fill(const String &tmpl) {
  String out;
  int start = 0;
  for (;;) {
    int bar = tmpl.indexOf('|', start);
    out += fillTmplLine(tmpl.substring(start, bar < 0 ? tmpl.length() : bar));
    if (bar < 0) break;
    out += '|';
    start = bar + 1;
  }
  return out;
}

static String wantedScreen() {
  Lock l;
  if (testText.length()) return testText;
  if (msg.length() && (int32_t)(msgUntil - millis()) > 0) return msg;
  if (sel.length()) {
    int n = sel.toInt();
    String door = slotId(n, "door"), area = slotId(n, "area");
    if (door.length()) return "Door " + sel + " " + nameOf(inc.doorNames, door, "") + "|Code: ";
    return "Area " + sel + " " + nameOf(inc.areaNames, area, "") + " (" + (areaState(area) & A_ARMED ? "armed" : "disarmed") + ")|Code: ";
  }
  if (gApMode && !WiFi.isConnected()) return "Wi-Fi setup: join network|" + gApName;
  int st = areaState(primaryArea());
  if (st & A_ALARM) return "ALARM " + nameOf(inc.areaNames, primaryArea(), "") + "|Code: ";
  if (!inc.faults.empty()) return fill(cfg.fault_message.length() ? cfg.fault_message : "FAULT: {faults}|Code: ");
  int areas = 0;
  for (int n = 1; n <= KEYPAD_SLOTS; n++) if (slotId(n, "area").length()) areas++;
  if (areas > 1) return fill(cfg.idle_message);      // several areas: always the home screen ({areas} shows them)
  return fill(st & A_ARMED ? cfg.armed_message : cfg.idle_message);
}

String kpScreen() { return ascii(wantedScreen()); }

void kpShow(const String &text, uint32_t secs) {
  Lock l;
  if (secs) { msg = text; msgUntil = millis() + secs * 1000; }
  shownValid = false;
}

void kpDone() { Lock l; busy = false; }

// ---------------------------------------------------------------- LEDs
void ledBytes(uint8_t v[7]) {
  Lock l;
  memset(v, 0, 7);
  for (int i = 0; i < LED_COUNT; i++) {
    auto it = cfg.leds.find(ledName(i));
    if (it == cfg.leds.end()) continue;
    int best = 0;
    for (auto &src : it->second.sources) {
      if (src.startsWith("area:")) {
        int st = areaState(src.substring(5));
        best = max(best, st & (A_ALARM | A_ENTRY | A_EXIT) ? 2 : st & A_ARMED ? 1 : 0);
      } else if (src.startsWith("input:")) {
        auto s = inc.inputStates.find(src.substring(6));
        if (s != inc.inputStates.end() && (s->second & I_ACTIVE)) best = max(best, it->second.flash ? 2 : 1);
      }
    }
    v[1 + i / 4] |= best << ((i % 4) * 2);
  }
}

// LED test: light one bit at a time across the 7 LED bytes, 4 s each; the screen says which bit is on.
static void ledTestStep() {
  Lock l;
  if (testStart < 0) return;
  int step = (millis() - testStart) / 4000;
  if (step >= 56) { testStart = -1; ledOverride = false; testText = ""; logf("LED test finished"); return; }
  memset(ledOv, 0, 7);
  ledOv[step / 8] = 1 << (step % 8);
  ledOverride = true;
  char t[48];
  snprintf(t, sizeof t, "LED TEST byte %d|bit %d (%02x)", step / 8, step % 8, 1 << (step % 8));
  testText = t;
}

void kpLedTest(const String &action, const String &bytes) {
  Lock l;
  if (action == "start" && testStart < 0) { testStart = millis(); logf("LED test started"); }
  else if (action == "stop") { testStart = -1; ledOverride = false; testText = ""; }
  else if (action == "set") {
    uint8_t v[7];
    int n = 0;
    const char *p = bytes.c_str();
    while (*p && n < 8) {
      while (*p == ' ') p++;
      if (!*p) break;
      char *end;
      long b = strtol(p, &end, 16);
      if (end == p) { n = 0; break; }
      if (n < 7) v[n] = b;
      n++;
      p = end;
    }
    if (n == 7) { memcpy(ledOv, v, 7); ledOverride = true; testText = ""; logf("LED bytes set by hand"); }
    else { ledOverride = false; logf("LED override cleared"); }
  }
  shownValid = false;
}

void kpStats(JsonObject out) {
  Lock l;
  out["polls"] = polls;
  out["replies"] = replies;
}

// ---------------------------------------------------------------- keys
static void clearEntry() {
  if (entry.length() || sel.length()) shownValid = false;
  entry = ""; sel = "";
  Lock l;
  if (msg.length()) { msg = ""; shownValid = false; }
}

static void action(bool arm) {
  String pin = entry, slot;
  entry = "";
  {
    Lock l;
    slot = cfg.slots[sel.length() ? sel.toInt() : 1];
  }
  sel = "";
  if (!slot.length()) { kpShow("No area set|Code: ", 3); return; }
  {
    Lock l;
    if (busy) return;
    busy = true;
  }
  bool door = slot.startsWith("door:");
  kpShow(door ? (arm ? "Opening door...|" : "Locking door...|") : (arm ? "Arming...|" : "Disarming...|"), 15);
  if (!inceptionQueue(arm, pin, slot)) kpDone();
}

static void key(uint8_t k) {
  bool digit = k >= 0x30 && k <= 0x39;
  String name = digit ? String("digit") : String(keyName(k));
  logf("KEY %s", name.c_str());
  lastKey = millis();
  if (digit) {
    if (entry.length() < 12) entry += (char)k;
    { Lock l; msg = ""; }
    todo.push_back({0x10, 0x01, 0x01, 0x2a});           // echo '*'
    return;
  }
  String slot, keyMode;
  {
    Lock l;
    slot = cfg.slots[sel.length() ? sel.toInt() : 1];
    keyMode = cfg.key_mode;
  }
  bool door = slot.startsWith("door:");
  if (name == "CLEAR") clearEntry();
  else if (name == "ON") action(true);
  else if (name == "OFF") action(false);
  // 12-key keypad: '#' sends ENTER, '*' sends MENU
  else if (name == "ENTER" && entry.length() && entry.length() <= 2 && !sel.length()) {
    int n = entry.toInt();                               // keypad area number, e.g. "2 #"
    entry = "";
    bool used;
    { Lock l; used = n >= 1 && n <= KEYPAD_SLOTS && cfg.slots[n].length(); }
    if (used) { sel = String(n); Lock l; msg = ""; shownValid = false; }
    else kpShow("No area " + String(n) + "|Code: ", 3);
  } else if (name == "ENTER" && entry.length()) {
    if (door || keyMode != "toggle") action(true);
    else { Lock l; String id = slot.substring(5); bool armed = areaState(id) & A_ARMED; action(!armed); }
  } else if (name == "MENU") {
    if (entry.length() && keyMode != "toggle") action(false);
    else clearEntry();
  }
}

// Keypad key message: 07 N k1..kN CHK. Every valid message must be acked or the keypad stops sending keys.
static void handleReply(const uint8_t *r, int n, uint32_t t) {
  if (n == 0 || (n == 1 && r[0] == 0x00)) return;
  if (r[0] != 0x07 || n < 3 || n < r[1] + 3) {
    char hex[100] = "";
    for (int i = 0; i < n && i < 30; i++) sprintf(hex + strlen(hex), "%02x ", r[i]);
    logf("odd reply %s", hex);
    return;
  }
  int cnt = r[1];
  uint8_t x = 0;
  for (int i = 0; i < cnt + 2; i++) x ^= r[i];
  if (r[cnt + 2] != (uint8_t)~x) { logf("bad checksum on key message"); return; }
  sendBody({0x10, 0x10});
  if (t - started < 3000) { logf("discarded %d keys buffered before start-up", cnt); return; }
  for (int i = 0; i < cnt; i++) key(r[2 + i]);
}

static void flushScreen(const String &text) {
  std::vector<uint8_t> body = {0x10, 0x01, 0x00, 0x0b};
  for (size_t i = 0; i < text.length() && body.size() < 74; i++) body.push_back(text[i] == '|' ? 0x0a : text[i]);
  body[2] = body.size() - 3;
  sendBody(body.data(), body.size());
  uint8_t r[64];
  readReply(r, sizeof r);
  shown = text;
  Lock l;
  shownValid = true;
}

static void task(void *) {
  uint8_t r[64];
  started = millis();
  logf("keypad bus running (RX GPIO%d, TX GPIO%d)", BUS_RX_PIN, BUS_TX_PIN);
  for (;;) {
    uint32_t t = millis();
    if ((entry.length() || sel.length()) && t - lastKey > 20000) clearEntry();
    ledTestStep();
    uint8_t led[7];
    bool ovr;
    {
      Lock l;
      ovr = ledOverride;
      if (ovr) memcpy(led, ledOv, 7);
    }
    if (!ovr) ledBytes(led);
    String want = kpScreen();
    bool valid;
    { Lock l; valid = shownValid; }
    if (!todo.empty()) {
      auto b = todo.front();
      todo.pop_front();
      sendBody(b.data(), b.size());
      readReply(r, sizeof r);
    } else if (!entry.length() && (!valid || want != shown)) {
      flushScreen(want);
    } else if (!ledValid || memcmp(led, ledSent, 7) || t - lastLed > (ovr ? 2000u : 31000u)) {
      std::vector<uint8_t> b = {0x10, 0x11, 0x07};
      b.insert(b.end(), led, led + 7);
      sendBody(b.data(), b.size());
      readReply(r, sizeof r);
      memcpy(ledSent, led, 7);
      ledValid = true;
      lastLed = t;
    } else {
      sendBody({0x90, 0x00});
      int n = readReply(r, sizeof r);
      {
        Lock l;
        polls++;
        if (n) replies++;
      }
      handleReply(r, n, t);
    }
    delay(5);
    sendBody({0xa1, 0x00, 0x80});
    while (millis() - t < CYCLE_MS) delay(2);
  }
}

void keypadBegin() {
  if (BUS_DE_PIN >= 0) { pinMode(BUS_DE_PIN, OUTPUT); digitalWrite(BUS_DE_PIN, LOW); }
  Bus.begin(4800, SERIAL_8N1, BUS_RX_PIN, BUS_TX_PIN);
  gpio_pullup_en((gpio_num_t)BUS_RX_PIN);      // a disconnected RX pin would otherwise pick up noise
  uart_set_rx_full_threshold(UART_NUM_2, 1);   // hand each byte over at once (default waits ~20 ms at 4800 baud)
  uart_set_rx_timeout(UART_NUM_2, 1);
  xTaskCreatePinnedToCore(task, "keypad", 8192, nullptr, 3, nullptr, 1);
}
