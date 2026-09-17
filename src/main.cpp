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
static const uint32_t POLL_MS = 30000;          // poll + view-flip cadence
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

// Rolling history for the chart: hashrate + temperature (one sample per poll).
static const int HIST = 180;
float hist[HIST];       // hashrate GH/s
float thist[HIST];      // temp C (dotted overlay)
int   histCount = 0;
void pushSample(float hr, float t) {
  if (histCount < HIST) { hist[histCount] = hr; thist[histCount] = t; histCount++; }
  else {
    memmove(hist,  hist  + 1, (HIST - 1) * sizeof(float));
    memmove(thist, thist + 1, (HIST - 1) * sizeof(float));
    hist[HIST - 1] = hr; thist[HIST - 1] = t;
  }
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

// The display alternates two views every poll; each swap changes the whole
// screen so a full (ghost-free) refresh is always used.

// Short label for a hashrate value on the Y axis (e.g. "512" GH, "1.2T").
String chartVal(float gh) {
  char b[10];
  if (gh >= 1000.0f) snprintf(b, sizeof(b), "%.1fT", gh / 1000.0f);
  else               snprintf(b, sizeof(b), "%.0f", gh);
  return String(b);
}

// Draw the hashrate history as a line chart with dynamic X (time) / Y (hashrate)
// gridlines and axis labels inside the area [ax,ay,aw,ah].
void drawChart(int ax, int ay, int aw, int ah) {
  const int GUT  = 24;   // left gutter for Y (hashrate) labels
  const int XLAB = 9;    // bottom strip for X (time) labels
  const int px = ax + GUT, py = ay, pw = aw - GUT, ph = ah - XLAB;
  const int baseY = py + ph, rightX = px + pw - 1;
  const int pollS = POLL_MS / 1000;

  display.setFont(NULL);
  display.setTextSize(1);
  auto dotH = [&](int x, int y, int w) { for (int i = 0; i < w; i += 3) display.drawPixel(x + i, y, GxEPD_BLACK); };
  auto dotV = [&](int x, int y, int h) { for (int i = 0; i < h; i += 3) display.drawPixel(x, y + i, GxEPD_BLACK); };

  // axes
  display.drawFastVLine(px, py, ph, GxEPD_BLACK);
  display.drawFastHLine(px, baseY, pw, GxEPD_BLACK);

  // Smoothed sample (centered moving average) so the line rounds off spikes.
  auto smooth = [&](int i) -> float {
    const int w = 2;                 // +/- 2 = 5-point average
    float s = 0; int n = 0;
    for (int j = i - w; j <= i + w; j++)
      if (j >= 0 && j < histCount) { s += hist[j]; n++; }
    return n ? s / n : hist[i];
  };

  float mn = 0, mx = 1;
  if (histCount >= 1) {
    mn = mx = smooth(0);
    for (int i = 1; i < histCount; i++) { float v = smooth(i); mn = min(mn, v); mx = max(mx, v); }
  }
  // Pad the Y range so a flat / low-variation line floats mid-plot (never glued
  // to the baseline axis where it would be invisible).
  float pad = max((mx - mn) * 0.20f, 2.0f);
  float lo = mn - pad, hi = mx + pad;

  // Y gridlines + labels at min / mid / max
  for (int k = 0; k <= 2; k++) {
    float frac = k / 2.0f;
    int yy = baseY - (int)(frac * (ph - 1));
    if (k > 0) dotH(px + 1, yy, pw - 1);
    String lab = chartVal(lo + (hi - lo) * frac);
    display.setCursor(px - 2 - lab.length() * 6, yy - 3);
    display.print(lab);
  }

  // X gridlines + time-ago labels at oldest / mid / now
  int spanS = (histCount > 1) ? (histCount - 1) * pollS : 0;
  for (int k = 0; k <= 2; k++) {
    float frac = k / 2.0f;                       // 0=oldest(left) .. 1=now(right)
    int xx = px + (int)(frac * (pw - 1));
    if (k < 2) dotV(xx, py, ph);
    int agoS = (int)((1.0f - frac) * spanS);
    String lab = (k == 2) ? String("now") : String("-") + String((agoS + 30) / 60) + "m";
    int lw = lab.length() * 6, lx = xx - lw / 2;
    lx = max(px, min(lx, rightX - lw));
    display.setCursor(lx, baseY + 2);
    display.print(lab);
  }

  if (histCount >= 2) {
    auto X = [&](int i) { return px + i * (pw - 1) / (histCount - 1); };

    // dotted temperature overlay (own auto-range -> secondary scale)
    float tmn = thist[0], tmx = thist[0];
    for (int i = 1; i < histCount; i++) { tmn = min(tmn, thist[i]); tmx = max(tmx, thist[i]); }
    float tpad = max((tmx - tmn) * 0.20f, 1.0f);
    float tlo = tmn - tpad, thi = tmx + tpad;
    auto TY = [&](int i) { return baseY - (int)((thist[i] - tlo) / (thi - tlo) * (ph - 1)); };
    for (int i = 0; i < histCount; i += 3)                // sparse dots = temp line
      display.drawPixel(X(i), TY(i), GxEPD_BLACK);
    // degree-C marker at the temp line's current end
    int lastTy = constrain(TY(histCount - 1), py + 5, baseY - 2);
    display.setFont(NULL); display.setTextSize(1);
    display.drawCircle(rightX - 9, lastTy - 4, 1, GxEPD_BLACK);   // degree ring
    display.setCursor(rightX - 6, lastTy - 4); display.print("C");

    // solid smoothed hashrate line
    auto Y = [&](int i) { return baseY - (int)((smooth(i) - lo) / (hi - lo) * (ph - 1)); };
    for (int i = 1; i < histCount; i++)
      display.drawLine(X(i - 1), Y(i - 1), X(i), Y(i), GxEPD_BLACK);
  }
}

// --- stat icons -------------------------------------------------------------
// Hand-drawn 16x16 icons for the big stats view (2 bytes/row, MSB = leftmost).
static const uint8_t ic16_thermo[] PROGMEM = {
  0x03,0xC0, 0x02,0x40, 0x02,0x40, 0x02,0x40, 0x02,0x40, 0x03,0xC0, 0x03,0xC0, 0x03,0xC0,
  0x03,0xC0, 0x07,0xE0, 0x0F,0xF0, 0x0F,0xF0, 0x0F,0xF0, 0x0F,0xF0, 0x07,0xE0, 0x03,0xC0 };
static const uint8_t ic16_bolt[] PROGMEM = {
  0x00,0x00, 0x01,0xF0, 0x03,0xE0, 0x07,0xC0, 0x0F,0x80, 0x1F,0xF8, 0x0F,0xF0, 0x01,0xF0,
  0x03,0xE0, 0x07,0xC0, 0x0F,0x80, 0x1F,0x00, 0x3E,0x00, 0x3C,0x00, 0x00,0x00, 0x00,0x00 };
static const uint8_t ic16_chip[] PROGMEM = {
  0x00,0x00, 0x00,0x00, 0x00,0x00, 0x1F,0xF8, 0x10,0x08, 0x90,0x09, 0x10,0x08, 0x90,0x09,
  0x10,0x08, 0x90,0x09, 0x10,0x08, 0x90,0x09, 0x10,0x08, 0x1F,0xF8, 0x00,0x00, 0x00,0x00 };
static const uint8_t ic16_leaf[] PROGMEM = {   // almond leaf w/ center vein + stem
  0x01,0x00, 0x02,0x80, 0x04,0x40, 0x09,0x20, 0x09,0x20, 0x11,0x10, 0x11,0x10, 0x11,0x10,
  0x09,0x20, 0x09,0x20, 0x05,0x40, 0x04,0x40, 0x02,0x80, 0x01,0x00, 0x01,0x00, 0x01,0x00 };
static const uint8_t ic16_wave[] PROGMEM = {
  0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x20,0x20, 0x70,0x70, 0x50,0x50,
  0x88,0x88, 0x88,0x88, 0x05,0x05, 0x0E,0x0E, 0x02,0x02, 0x00,0x00, 0x00,0x00, 0x00,0x00 };
static const uint8_t ic16_clock[] PROGMEM = {
  0x00,0x00, 0x03,0xC0, 0x0C,0x30, 0x10,0x08, 0x20,0x04, 0x20,0x04, 0x40,0x02, 0x41,0x02,
  0x41,0x82, 0x40,0xE2, 0x20,0x04, 0x20,0x04, 0x10,0x08, 0x0C,0x30, 0x03,0xC0, 0x00,0x00 };

// 8x8 icons for the graph view's shares/best strip.
static const uint8_t ic_check[] PROGMEM = {
  0b00000000, 0b00000011, 0b00000110, 0b00001100,
  0b11011000, 0b01110000, 0b00100000, 0b00000000 };
static const uint8_t ic_star[] PROGMEM = {
  0b00011000, 0b00011000, 0b11111111, 0b01111110,
  0b00111100, 0b01100110, 0b01000010, 0b00000000 };

// Shared header (both views): current hashrate (left, bold) + RSSI dBm (right).
void drawHeader() {
  const int W = display.width();
  char b[16];
  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(4, 18); display.print(fmtHash(st.hashRate));
  snprintf(b, sizeof(b), "%d dBm", st.rssi);
  display.setFont(&FreeSans9pt7b);
  display.setCursor(W - textW(b) - 4, 16); display.print(b);
  display.drawFastHLine(0, 22, W, GxEPD_BLACK);
}

// View A: header + hashrate chart (dotted temp overlay) + shares / best strip.
void drawGraphView() {
  renderFrame([&]() {
    const int W = display.width();
    char b[24];
    drawHeader();
    drawChart(2, 32, W - 4, 74);       // graph pushed down from header, y32..106
    display.setFont(NULL);
    display.setTextSize(1);
    display.drawBitmap(4, 114, ic_check, 8, 8, GxEPD_BLACK);
    snprintf(b, sizeof(b), "%ld/%ld", st.sharesAccepted, st.sharesRejected);
    display.setCursor(15, 114); display.print(b);
    display.drawBitmap(150, 114, ic_star, 8, 8, GxEPD_BLACK);
    display.setCursor(161, 114); display.print("Best " + fmtDiff(st.bestDiff));
  }, true);
}

// View B: header + big-icon stats grid (2 columns x 3 rows).
void drawStatsView() {
  renderFrame([&]() {
    char b[16];
    drawHeader();
    display.setFont(&FreeSans9pt7b);
    const int LX = 8, RX = 156;
    const int ry[3] = {34, 68, 102};   // icon top y (pushed down from header)
    auto cell = [&](int x, int y, const uint8_t* ic, const String& val) {
      display.drawBitmap(x, y, ic, 16, 16, GxEPD_BLACK);
      display.setCursor(x + 22, y + 12); display.print(val);
    };
    snprintf(b, sizeof(b), "%.1f C", st.temp);      cell(LX, ry[0], ic16_thermo, b);
    snprintf(b, sizeof(b), "%.1f W", st.power);     cell(RX, ry[0], ic16_bolt,   b);
    snprintf(b, sizeof(b), "%.0f C", st.vrTemp);    cell(LX, ry[1], ic16_chip,   b);
    snprintf(b, sizeof(b), "%d J/TH", effJTH());    cell(RX, ry[1], ic16_leaf,   b);
    snprintf(b, sizeof(b), "%d MHz", st.frequency); cell(LX, ry[2], ic16_wave,   b);
    cell(RX, ry[2], ic16_clock, fmtUptime(st.uptimeSeconds));
  }, true);
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
      pushSample(st.hashRate, st.temp);
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

  if (fetchStats()) drawGraphView();
  else drawMessage("Bitaxe offline", bitaxeIp, "retrying...");
}

uint32_t lastPoll = 0;
int view = 0;                          // 0 = graph, 1 = stats
void loop() {
  if (millis() - lastPoll >= POLL_MS || lastPoll == 0) {
    lastPoll = millis();
    if (fetchStats()) {
      if (view == 0) drawGraphView(); else drawStatsView();
      view ^= 1;                       // flip view each poll
      Serial.printf("[paper-display] %.1f GH/s  %.1fC  %.1fW\n",
                    st.hashRate, st.temp, st.power);
    } else if (!st.valid) {
      drawMessage("Bitaxe offline", bitaxeIp, "check IP / power");
    }
  }
  delay(200);
}
