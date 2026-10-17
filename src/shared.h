// Shared settings, Inception state and helpers for the keypad bridge.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <map>
#include <set>
#include <vector>

// ---------------------------------------------------------------- pins (change here if wired differently)
#define BUS_RX_PIN 16       // RS485 module RXD / RO  -> ESP32 GPIO16
#define BUS_TX_PIN 17       // RS485 module TXD / DI  -> ESP32 GPIO17
#define BUS_DE_PIN -1       // DE+RE pin for MAX485-style modules; -1 = auto-direction module (no DE pin)
#define BOOT_BUTTON_PIN 0   // hold BOOT for 5 s to forget the Wi-Fi settings

#define FW_VERSION "1.0.0"
#define LCD_W 40            // characters per keypad display line
#define KEYPAD_SLOTS 8      // keypad "area numbers" 1..8

// Inception public state bits
#define A_ARMED 0x1
#define A_ALARM 0x2
#define A_ENTRY 0x4
#define A_EXIT 0x8
#define I_ACTIVE 0x1

struct LedCfg {
  std::vector<String> sources;   // "area:<id>" or "input:<id>"
  bool flash = false;            // inputs: flash instead of solid while active
};

struct Config {
  // device
  String wifi_ssid, wifi_pass;
  String hostname = "keypad-bridge";
  String tz = "AEST-10AEDT,M10.1.0,M4.1.0/3";   // POSIX time zone, default Sydney/Melbourne
  String ntp = "pool.ntp.org";
  String web_pass;                               // optional settings-page password (user "admin")
  // Inception
  String inception_host, username, password;
  // keypad
  String pin_mode = "local";                     // "local" or "inception"
  String local_pins;                             // comma separated
  String key_mode = "labels";                    // "labels" (# arm, * disarm) or "toggle"
  String door_action = "Open";                   // what "code #" does on a door slot: Open or Unlock
  String idle_message = "{date} {time}|Code: ";
  String armed_message = "{area} ARMED|Code: ";
  String fault_message = "FAULT: {faults}|Code: ";
  String slots[KEYPAD_SLOTS + 1];                // [1..8] = "area:<id>", "door:<id>" or ""
  std::map<String, LedCfg> leds;                 // LED name -> sources
  bool show_custom = false;                      // include custom inputs in {unsealed}
};

struct IncState {
  std::map<String, int> areaStates, inputStates;
  std::map<String, String> areaNames, inputNames, doorNames;
  std::set<String> custom;
  std::vector<String> openInputs, faults;
  String error = "not connected yet";
  bool namesLoaded = false;
};

extern Config cfg;
extern IncState inc;
extern SemaphoreHandle_t gLock;   // guards cfg, inc and the keypad's shared fields

struct Lock {
  Lock() { xSemaphoreTakeRecursive(gLock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(gLock); }
};

// LED names in packet order: d1 COMMS RAS DGP BATTERY, d2 ACCESS 24HR SECURE TAMPER, d3..d6 areas 1-16
extern const char *const SYS_LEDS[8];
String ledName(int i);            // 0..23
static const int LED_COUNT = 24;

// config.cpp
bool loadConfig();
bool saveConfig();
void configToJson(JsonDocument &doc);                    // without secrets
bool configFromJson(JsonObjectConst in, bool &needReboot);
String slotId(int n, const char *kind);                  // id when slot n is of that kind, else ""
String primaryArea();                                    // area of slot 1 (used by {area}, {state})

// log.cpp
void logf(const char *fmt, ...);
void logLines(JsonArray out, int max);

// inception.cpp
void inceptionBegin();
bool inceptionQueue(bool arm, const String &pin, const String &slot);   // slot = "area:<id>" / "door:<id>"
bool inceptionTest(const String &host, const String &user, const String &pass, String &msg);
void inceptionReloadNames();

// keypad.cpp
void keypadBegin();
void kpShow(const String &text, uint32_t secs = 0);
void kpDone();                     // the queued Inception action has finished
String kpScreen();
void kpStats(JsonObject out);
void kpLedTest(const String &action, const String &bytes);
void ledBytes(uint8_t v[7]);

// web.cpp
void webBegin();
void webStartPortal();
void webStopPortal();
void webLoop();
extern bool gApMode;
extern String gApName;
