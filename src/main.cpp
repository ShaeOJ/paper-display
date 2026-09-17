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
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

// ---- e-paper -------------------------------------------------------------
#define EPD_CS   15
#define EPD_DC    4
#define EPD_RST   5
// BUSY on GPIO16 reads as perpetually-busy on this board, so GxEPD2 would stall
// the full 10s timeout every refresh. Setting BUSY=-1 makes GxEPD2 use short,
// per-operation timed delays instead — no stall, smooth refresh. (Set back to 16
// only if a board actually drives BUSY correctly.)
#define EPD_BUSY -1

// Heltec 2.9" panel = GDEM029T94 controller: use the T94_V2 class for fast
// partial (flicker-free) refresh. If you see a blank/garbled screen or a slow
// ~10s "Busy Timeout" per refresh, fall back to GxEPD2_290_BS.
GxEPD2_BW<GxEPD2_290_T94_V2, GxEPD2_290_T94_V2::HEIGHT>
    display(GxEPD2_290_T94_V2(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

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

// Rolling hashrate history for the sparkline (one sample per poll).
static const int HIST = 180;
float hist[HIST];
int   histCount = 0;
void pushHist(float v) {
  if (histCount < HIST) hist[histCount++] = v;
  else { memmove(hist, hist + 1, (HIST - 1) * sizeof(float)); hist[HIST - 1] = v; }
}

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

// Format a difficulty for display as a compact number with a K/M/G/T/P suffix.
// AxeOS may return bestDiff already suffixed (e.g. "116M") or as a raw number;
// handle both. A pre-suffixed string is returned uppercased as-is.
String fmtDiff(const String& raw) {
  if (raw.length() == 0) return String("0");
  char last = raw[raw.length() - 1];
  if (isalpha(last)) { String s = raw; s.toUpperCase(); return s; }
  double v = raw.toDouble();
  const char* suf[] = {"", "K", "M", "G", "T", "P"};
  int i = 0;
  while (v >= 1000.0 && i < 5) { v /= 1000.0; i++; }
  char b[16];
  if (v >= 100 || i == 0) snprintf(b, sizeof(b), "%.0f%s", v, suf[i]);
  else                    snprintf(b, sizeof(b), "%.1f%s", v, suf[i]);
  return String(b);
}

// Width of a string in the currently selected font.
uint16_t textW(const String& s) {
  int16_t x, y; uint16_t w, h;
  display.getTextBounds(s, 0, 0, &x, &y, &w, &h);
  return w;
}

// Trim a string (current font already set) until it fits within maxpx.
String fitW(String s, uint16_t maxpx) {
  while (s.length() > 1 && textW(s) > maxpx) s.remove(s.length() - 1);
  return s;
}

// ---- rendering -----------------------------------------------------------
// Render with the given body-builder. `full`=true does a clean full-window
// refresh (clears ghosting); `full`=false does a fast partial refresh (smooth,
// no black flash) on panels that support it. A periodic full refresh de-ghosts.
void renderFrame(std::function<void()> body, bool full) {
  if (full) display.setFullWindow();
  else      display.setPartialWindow(0, 0, display.width(), display.height());
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
  }, true);
}

// e-paper trade-off: a FULL refresh is crisp but blinks the whole panel; a
// PARTIAL refresh is blink-free but leaves faint ghosting that builds up. So we
// do partial updates every poll (smooth, no blink) and a full clean-up refresh
// every FULL_EVERY polls (default 12 = ~6 min at 30s) to clear ghosting.
// FULL_EVERY=1 => always full (no ghost, blinks every poll).
static const int FULL_EVERY = 12;
int drawCount = 0;

// Draw the hashrate sparkline within [gx,gy,gw,gh] as a gradient-filled area
// chart. True gradients aren't possible on 1-bit e-paper, so the fill under the
// curve uses a 4x4 ordered (Bayer) dither whose density fades from dense at the
// baseline to sparse near the line, with a crisp solid line drawn on top.
void drawSparkline(int gx, int gy, int gw, int gh) {
  const int baseY = gy + gh;
  display.drawFastHLine(gx, baseY, gw, GxEPD_BLACK);      // baseline axis
  if (histCount < 2) return;

  float mn = hist[0], mx = hist[0];
  for (int i = 1; i < histCount; i++) { mn = min(mn, hist[i]); mx = max(mx, hist[i]); }
  if (mx - mn < 1.0f) { mx = mn + 1.0f; }                // flat -> avoid /0

  static const uint8_t bayer[4][4] = {
    { 0,  8,  2, 10}, {12,  4, 14,  6}, { 3, 11,  1,  9}, {15,  7, 13,  5}
  };
  // Hashrate value interpolated at a given pixel column (0..gw-1).
  auto valAt = [&](int col) -> float {
    if (gw <= 1) return hist[histCount - 1];
    float f = (float)col * (histCount - 1) / (gw - 1);
    int i0 = (int)f;
    if (i0 >= histCount - 1) return hist[histCount - 1];
    float fr = f - i0;
    return hist[i0] * (1.0f - fr) + hist[i0 + 1] * fr;
  };

  int prevCy = -1;
  for (int col = 0; col < gw; col++) {
    int x = gx + col;
    float v = valAt(col);
    int cy = gy + gh - 1 - (int)((v - mn) / (mx - mn) * (gh - 1));
    // gradient fill: dense (dark) at baseline, fading up toward the line
    for (int y = cy; y < baseY; y++) {
      float t = (float)(baseY - y) / (float)gh;          // 0 at baseline, 1 at top
      uint8_t level = (uint8_t)((1.0f - t) * 16.0f);     // 16=solid .. 0=empty
      if (bayer[x & 3][y & 3] < level) display.drawPixel(x, y, GxEPD_BLACK);
    }
    // crisp line on top, connected across columns
    if (prevCy >= 0) display.drawLine(x - 1, prevCy, x, cy, GxEPD_BLACK);
    else             display.drawPixel(x, cy, GxEPD_BLACK);
    prevCy = cy;
  }
}

void drawStats() {
  bool full = (drawCount % FULL_EVERY == 0);
  drawCount++;
  renderFrame([&]() {
    const int W = display.width();     // 296
    char b[28];

    // --- header: current hashrate (left, bold) + RSSI (right) ---
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(4, 17); display.print(fmtHash(st.hashRate));
    snprintf(b, sizeof(b), "%d dBm", st.rssi);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(W - textW(b) - 4, 15); display.print(b);

    // --- sparkline (hashrate history) ---
    drawSparkline(3, 22, W - 6, 44);   // y 22..66

    // --- compact stats grid: tiny built-in font, 2 columns x 4 rows ---
    display.setFont(NULL);
    display.setTextSize(1);
    const int LX = 4, RX = 152;
    const int ys[4] = {78, 90, 102, 114};
    auto cell = [&](int x, int y, const String& s) { display.setCursor(x, y); display.print(s); };

    snprintf(b, sizeof(b), "Temp %.1fC", st.temp);       cell(LX, ys[0], b);
    snprintf(b, sizeof(b), "Pwr  %.1fW", st.power);      cell(RX, ys[0], b);
    snprintf(b, sizeof(b), "VR   %.0fC", st.vrTemp);     cell(LX, ys[1], b);
    snprintf(b, sizeof(b), "Eff  %d J/TH", effJTH());    cell(RX, ys[1], b);
    snprintf(b, sizeof(b), "Freq %dMHz", st.frequency);  cell(LX, ys[2], b);
    snprintf(b, sizeof(b), "A/R  %ld/%ld",
             st.sharesAccepted, st.sharesRejected);      cell(RX, ys[2], b);
    cell(LX, ys[3], "Best " + fmtDiff(st.bestDiff));
    cell(RX, ys[3], "Up   " + fmtUptime(st.uptimeSeconds));
  }, full);
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
      pushHist(st.hashRate);
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
