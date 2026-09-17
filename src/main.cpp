// paper-display — Bitaxe (AxeOS) live stats on a 2.9" B/W e-paper.
//
// Polls a Bitaxe's http://<ip>/api/system/info and renders hashrate, temp,
// power, efficiency, frequency, shares, and best difficulty to a 296x128
// e-paper on the Waveshare e-Paper ESP8266 Driver Board.
//
// First boot (or if WiFi fails) opens a captive-portal AP named "paper-display":
// join it, pick your WiFi, and enter the Bitaxe IP. Settings persist in LittleFS.
//
// Driver-board pin map (fixed by the PCB): BUSY=16 RST=5 DC=4 CS=15 SCK=14 MOSI=13.

#include <Arduino.h>
#include <functional>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <LittleFS.h>
#include <WiFiManager.h>          // tzapu/WiFiManager
#include <ArduinoJson.h>          // v6

#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

// ---- e-paper -------------------------------------------------------------
#define EPD_CS   15
#define EPD_DC    4
#define EPD_RST   5
#define EPD_BUSY 16

// Panel class confirmed working on the Heltec 2.9" B/W. If you swap panels and
// see a slow ~10s "Busy Timeout" per refresh, try GxEPD2_290_T94_V2 instead.
GxEPD2_BW<GxEPD2_290_BS, GxEPD2_290_BS::HEIGHT>
    display(GxEPD2_290_BS(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ---- config --------------------------------------------------------------
static const char* CFG_PATH   = "/config.json";
static const uint32_t POLL_MS = 30000;          // refresh cadence
char bitaxeIp[40] = "";
bool shouldSaveConfig = false;

// ---- live stats ----------------------------------------------------------
struct Stats {
  bool     valid = false;
  String   hostname;
  float    hashRate = 0;     // GH/s
  float    temp = 0;         // C (ASIC)
  float    vrTemp = 0;       // C (voltage regulator)
  float    power = 0;        // W
  int      frequency = 0;    // MHz
  long     sharesAccepted = 0;
  long     sharesRejected = 0;
  String   bestDiff;         // AxeOS returns this as a string, e.g. "116M"
  long     uptimeSeconds = 0;
  int      rssi = 0;
} st;

// ---- persistence ---------------------------------------------------------
void loadConfig() {
  if (!LittleFS.begin()) { LittleFS.format(); LittleFS.begin(); }
  File f = LittleFS.open(CFG_PATH, "r");
  if (!f) return;
  StaticJsonDocument<256> doc;
  if (!deserializeJson(doc, f)) {
    strlcpy(bitaxeIp, doc["bitaxeIp"] | "", sizeof(bitaxeIp));
  }
  f.close();
}

void saveConfig() {
  File f = LittleFS.open(CFG_PATH, "w");
  if (!f) return;
  StaticJsonDocument<256> doc;
  doc["bitaxeIp"] = bitaxeIp;
  serializeJson(doc, f);
  f.close();
}

void onSaveConfig() { shouldSaveConfig = true; }

// ---- formatting helpers --------------------------------------------------
String fmtHash(float gh) {
  char b[24];
  if (gh >= 1000.0f) snprintf(b, sizeof(b), "%.2f TH/s", gh / 1000.0f);
  else               snprintf(b, sizeof(b), "%.1f GH/s", gh);
  return String(b);
}

String fmtUptime(long s) {
  long d = s / 86400; s %= 86400;
  long h = s / 3600;  s %= 3600;
  long m = s / 60;
  char b[24];
  if (d > 0)      snprintf(b, sizeof(b), "%ldd %ldh", d, h);
  else if (h > 0) snprintf(b, sizeof(b), "%ldh %ldm", h, m);
  else            snprintf(b, sizeof(b), "%ldm", m);
  return String(b);
}

// efficiency in J/TH = watts / (GH/s / 1000)
int effJTH() {
  if (st.hashRate <= 0) return 0;
  return (int)lroundf(st.power / (st.hashRate / 1000.0f));
}

// ---- rendering -----------------------------------------------------------
// Draw a full frame with the given body-builder. Full-window update (no ghosting).
void renderFrame(std::function<void()> body) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    body();
  } while (display.nextPage());
}

void drawMessage(const char* title, const char* line1, const char* line2) {
  renderFrame([&]() {
    display.drawRect(2, 2, display.width() - 4, display.height() - 4, GxEPD_BLACK);
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(12, 30); display.print(title);
    display.setFont(&FreeSans9pt7b);
    if (line1) { display.setCursor(12, 62);  display.print(line1); }
    if (line2) { display.setCursor(12, 90);  display.print(line2); }
  });
}

void drawStats() {
  renderFrame([&]() {
    const int W = display.width();   // 296
    // header: hostname + rssi
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(4, 15);
    display.print(st.hostname.length() ? st.hostname : String("bitaxe"));
    char rssi[10]; snprintf(rssi, sizeof(rssi), "%ddBm", st.rssi);
    int16_t bx, by; uint16_t bw, bh;
    display.getTextBounds(rssi, 0, 0, &bx, &by, &bw, &bh);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(W - bw - 6, 15); display.print(rssi);
    display.drawFastHLine(4, 20, W - 8, GxEPD_BLACK);

    // hashrate (big)
    display.setFont(&FreeSansBold18pt7b);
    String h = fmtHash(st.hashRate);
    display.getTextBounds(h, 0, 0, &bx, &by, &bw, &bh);
    display.setCursor((W - bw) / 2, 52); display.print(h);

    // stat rows
    display.setFont(&FreeSans9pt7b);
    char buf[48];
    snprintf(buf, sizeof(buf), "%.1fC   %.1fW   %d J/TH", st.temp, st.power, effJTH());
    display.setCursor(6, 76);  display.print(buf);
    snprintf(buf, sizeof(buf), "%dMHz   VR %.0fC", st.frequency, st.vrTemp);
    display.setCursor(6, 96);  display.print(buf);
    snprintf(buf, sizeof(buf), "A:%ld R:%ld  Best %s",
             st.sharesAccepted, st.sharesRejected, st.bestDiff.c_str());
    display.setCursor(6, 116); display.print(buf);

    // footer: uptime
    display.drawFastHLine(4, 120, W - 8, GxEPD_BLACK);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(6, 126); display.print("up " + fmtUptime(st.uptimeSeconds));
  });
}

// ---- Bitaxe poll ---------------------------------------------------------
bool fetchStats() {
  if (WiFi.status() != WL_CONNECTED || strlen(bitaxeIp) == 0) return false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(5000);
  String url = "http://" + String(bitaxeIp) + "/api/system/info";
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, http.getString())) {
      st.hostname       = doc["hostname"].as<String>();
      st.hashRate       = doc["hashRate"].as<float>();
      st.temp           = doc["temp"].as<float>();
      st.vrTemp         = doc["vrTemp"].as<float>();
      st.power          = doc["power"].as<float>();
      st.frequency      = doc["frequency"].as<int>();
      st.sharesAccepted = doc["sharesAccepted"].as<long>();
      st.sharesRejected = doc["sharesRejected"].as<long>();
      st.bestDiff       = doc["bestDiff"].as<String>();
      st.uptimeSeconds  = doc["uptimeSeconds"].as<long>();
      st.rssi           = WiFi.RSSI();
      st.valid = true;
      ok = true;
    }
  }
  http.end();
  return ok;
}

// ---- setup / loop --------------------------------------------------------
void startPortal(bool onDemand) {
  WiFiManager wm;
  wm.setSaveConfigCallback(onSaveConfig);
  WiFiManagerParameter pIp("bitaxe", "Bitaxe IP (e.g. 10.0.0.x)", bitaxeIp, sizeof(bitaxeIp) - 1);
  wm.addParameter(&pIp);
  wm.setConfigPortalTimeout(180);

  bool connected = onDemand ? wm.startConfigPortal("paper-display")
                            : wm.autoConnect("paper-display");
  strlcpy(bitaxeIp, pIp.getValue(), sizeof(bitaxeIp));
  if (shouldSaveConfig) { saveConfig(); shouldSaveConfig = false; }
  if (!connected) { ESP.restart(); }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[paper-display] boot");

  display.init(115200);
  display.setRotation(1);              // 296x128 landscape
  drawMessage("paper-display", "Starting up...", "");

  loadConfig();

  // If no Bitaxe IP yet, show setup hint before opening the portal.
  if (strlen(bitaxeIp) == 0) {
    drawMessage("Setup needed", "Join WiFi AP:", "paper-display");
  }
  startPortal(false);                  // autoConnect: uses saved creds or portal

  Serial.printf("[paper-display] wifi=%s ip=%s bitaxe=%s\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), bitaxeIp);

  if (fetchStats()) drawStats();
  else drawMessage("Bitaxe offline", bitaxeIp, "retrying...");
}

uint32_t lastPoll = 0;
void loop() {
  if (millis() - lastPoll >= POLL_MS || lastPoll == 0) {
    lastPoll = millis();
    if (fetchStats()) {
      drawStats();
      Serial.printf("[paper-display] %.1f GH/s  %.1fC  %.1fW\n",
                    st.hashRate, st.temp, st.power);
    } else if (!st.valid) {
      drawMessage("Bitaxe offline", bitaxeIp, "check IP / power");
    }
  }
  delay(200);
}
