// paper-display — Bitaxe (AxeOS) live stats on a 2.9" B/W e-paper.
//
// Polls a Bitaxe's http://<ip>/api/system/info and renders hashrate, temp,
// power, efficiency, frequency, shares, and best difficulty to a 296x128
// e-paper on the Waveshare e-Paper ESP8266 Driver Board.
//
// First boot (or if WiFi fails) opens a captive-portal AP named "paper-display":
// join it, pick your WiFi, and enter one or more Bitaxe IPs (comma-separated).
// You can also set/replace the IP list any time over serial (see handleSerial).
// With >1 device it shows a fleet overview then rotates each device's detail.
// Settings persist in LittleFS.
//
// Driver-board pin map (fixed by the PCB): BUSY=16 RST=5 DC=4 CS=15 SCK=14 MOSI=13.

#include <Arduino.h>
#include <functional>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
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
static const uint32_t POLL_MS = 30000;          // poll + screen-flip cadence
static const int MAX_DEV = 6;
char ipList[200] = "";                           // raw comma-separated IPs (portal field)
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
};

// One monitored device: IP, latest stats, and rolling chart history.
static const int HIST = 180;
struct Dev {
  char  ip[24] = "";
  Stats st;
  float hist[HIST];          // hashrate GH/s history
  float thist[HIST];         // temp C history
  int   histCount = 0;
};
Dev devs[MAX_DEV];
int  devCount = 0;

void pushSample(Dev& d, float hr, float t) {
  if (d.histCount < HIST) { d.hist[d.histCount] = hr; d.thist[d.histCount] = t; d.histCount++; }
  else {
    memmove(d.hist,  d.hist  + 1, (HIST - 1) * sizeof(float));
    memmove(d.thist, d.thist + 1, (HIST - 1) * sizeof(float));
    d.hist[HIST - 1] = hr; d.thist[HIST - 1] = t;
  }
}

// Parse a comma-separated IP string into the device list.
void parseDevices(const char* csv) {
  devCount = 0;
  String s(csv);
  int start = 0;
  while (start <= (int)s.length() && devCount < MAX_DEV) {
    int c = s.indexOf(',', start);
    if (c < 0) c = s.length();
    String tok = s.substring(start, c); tok.trim();
    if (tok.length()) {
      Dev& d = devs[devCount];
      strlcpy(d.ip, tok.c_str(), sizeof(d.ip));
      d.histCount = 0; d.st = Stats();
      devCount++;
    }
    start = c + 1;
  }
}

// ---- persistence ---------------------------------------------------------
void loadConfig() {
  if (!LittleFS.begin()) { LittleFS.format(); LittleFS.begin(); }
  File f = LittleFS.open(CFG_PATH, "r");
  if (!f) return;
  StaticJsonDocument<512> doc;
  if (!deserializeJson(doc, f)) {
    const char* list = doc["bitaxeIps"] | "";        // new: comma-separated list
    if (strlen(list) == 0) list = doc["bitaxeIp"] | ""; // legacy: single IP
    strlcpy(ipList, list, sizeof(ipList));
  }
  f.close();
}

void saveConfig() {
  File f = LittleFS.open(CFG_PATH, "w");
  if (!f) return;
  StaticJsonDocument<512> doc;
  doc["bitaxeIps"] = ipList;
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
int effJTH(const Stats& s) {
  if (s.hashRate <= 0) return 0;
  return (int)lroundf(s.power / (s.hashRate / 1000.0f));
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
void drawChart(const Dev& d, int ax, int ay, int aw, int ah) {
  const int GUT  = 24;   // left gutter: hashrate labels
  const int RGUT = 22;   // right gutter: temp labels
  const int XLAB = 9;    // bottom strip: time labels
  const int px = ax + GUT, py = ay, pw = aw - GUT - RGUT, ph = ah - XLAB;
  const int baseY = py + ph, rightX = px + pw - 1;
  const int pollS = POLL_MS / 1000;

  display.setFont(NULL);
  display.setTextSize(1);
  auto dotH = [&](int x, int y, int w) { for (int i = 0; i < w; i += 3) display.drawPixel(x + i, y, GxEPD_BLACK); };
  auto dotV = [&](int x, int y, int h) { for (int i = 0; i < h; i += 3) display.drawPixel(x, y + i, GxEPD_BLACK); };

  // axes: left (hashrate), right (temp), bottom (time)
  display.drawFastVLine(px,     py, ph, GxEPD_BLACK);
  display.drawFastVLine(rightX, py, ph, GxEPD_BLACK);
  display.drawFastHLine(px, baseY, pw, GxEPD_BLACK);

  // 5-point moving average so the hashrate line rounds off spikes.
  auto smooth = [&](int i) -> float {
    const int w = 2;
    float s = 0; int n = 0;
    for (int j = i - w; j <= i + w; j++)
      if (j >= 0 && j < d.histCount) { s += d.hist[j]; n++; }
    return n ? s / n : d.hist[i];
  };

  // hashrate range (left axis)
  float mn = 0, mx = 1;
  if (d.histCount >= 1) {
    mn = mx = smooth(0);
    for (int i = 1; i < d.histCount; i++) { float v = smooth(i); mn = min(mn, v); mx = max(mx, v); }
  }
  float pad = max((mx - mn) * 0.20f, 2.0f);
  float lo = mn - pad, hi = mx + pad;

  // temp range (right axis)
  float tmn = 0, tmx = 1;
  if (d.histCount >= 1) {
    tmn = tmx = d.thist[0];
    for (int i = 1; i < d.histCount; i++) { tmn = min(tmn, d.thist[i]); tmx = max(tmx, d.thist[i]); }
  }
  float tpad = max((tmx - tmn) * 0.20f, 1.0f);
  float tlo = tmn - tpad, thi = tmx + tpad;

  // Y gridlines + dual labels: hashrate (left), temp (right; top label marked C)
  for (int k = 0; k <= 2; k++) {
    float frac = k / 2.0f;
    int yy = baseY - (int)(frac * (ph - 1));
    if (k > 0) dotH(px + 1, yy, pw - 2);
    String hl = chartVal(lo + (hi - lo) * frac);
    display.setCursor(px - 2 - hl.length() * 6, yy - 3); display.print(hl);
    char tl[8];
    if (k == 2) snprintf(tl, sizeof(tl), "%.0fC", tlo + (thi - tlo) * frac);
    else        snprintf(tl, sizeof(tl), "%.0f",  tlo + (thi - tlo) * frac);
    display.setCursor(rightX + 3, yy - 3); display.print(tl);
  }

  // X mid gridline + time-ago labels (oldest / mid / now)
  int spanS = (d.histCount > 1) ? (d.histCount - 1) * pollS : 0;
  for (int k = 0; k <= 2; k++) {
    float frac = k / 2.0f;
    int xx = px + (int)(frac * (pw - 1));
    if (k == 1) dotV(xx, py, ph);
    int agoS = (int)((1.0f - frac) * spanS);
    String lab = (k == 2) ? String("now") : String("-") + String((agoS + 30) / 60) + "m";
    int lw = lab.length() * 6, lx = xx - lw / 2;
    lx = max(px, min(lx, rightX - lw));
    display.setCursor(lx, baseY + 2); display.print(lab);
  }

  if (d.histCount >= 2) {
    auto X  = [&](int i) { return px + i * (pw - 1) / (d.histCount - 1); };
    auto TY = [&](int i) { return baseY - (int)((d.thist[i] - tlo) / (thi - tlo) * (ph - 1)); };
    auto HY = [&](int i) { return baseY - (int)((smooth(i)  - lo ) / (hi  - lo ) * (ph - 1)); };

    // thin temperature line (right-axis scale)
    for (int i = 1; i < d.histCount; i++)
      display.drawLine(X(i - 1), TY(i - 1), X(i), TY(i), GxEPD_BLACK);

    // bold hashrate line (drawn twice, 1px offset = 2px thick)
    for (int i = 1; i < d.histCount; i++) {
      display.drawLine(X(i - 1), HY(i - 1),     X(i), HY(i),     GxEPD_BLACK);
      display.drawLine(X(i - 1), HY(i - 1) - 1, X(i), HY(i) - 1, GxEPD_BLACK);
    }
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

// Per-device header: current hashrate (left, bold) + device name (right).
void drawHeader(const Dev& d) {
  const int W = display.width();
  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(4, 18); display.print(fmtHash(d.st.hashRate));
  display.setFont(&FreeSans9pt7b);
  String nm = d.st.hostname.length() ? d.st.hostname : String(d.ip);
  nm = fitW(nm, 120);
  display.setCursor(W - textW(nm) - 4, 16); display.print(nm);
  display.drawFastHLine(0, 22, W, GxEPD_BLACK);
}

// View A: header + dual-axis chart + shares / best strip (one device).
void drawGraphView(const Dev& d) {
  renderFrame([&]() {
    const int W = display.width();
    char b[24];
    drawHeader(d);
    drawChart(d, 2, 32, W - 4, 74);
    display.setFont(NULL);
    display.setTextSize(1);
    display.drawBitmap(4, 114, ic_check, 8, 8, GxEPD_BLACK);
    snprintf(b, sizeof(b), "%ld/%ld", d.st.sharesAccepted, d.st.sharesRejected);
    display.setCursor(15, 114); display.print(b);
    display.drawBitmap(150, 114, ic_star, 8, 8, GxEPD_BLACK);
    display.setCursor(161, 114); display.print("Best " + fmtDiff(d.st.bestDiff));
  }, true);
}

// View B: header + big-icon stats grid (one device).
void drawStatsView(const Dev& d) {
  renderFrame([&]() {
    char b[16];
    drawHeader(d);
    display.setFont(&FreeSans9pt7b);
    const int LX = 8, RX = 156;
    const int ry[3] = {34, 68, 102};
    auto cell = [&](int x, int y, const uint8_t* ic, const String& val) {
      display.drawBitmap(x, y, ic, 16, 16, GxEPD_BLACK);
      display.setCursor(x + 22, y + 12); display.print(val);
    };
    snprintf(b, sizeof(b), "%.1f C", d.st.temp);      cell(LX, ry[0], ic16_thermo, b);
    snprintf(b, sizeof(b), "%.1f W", d.st.power);     cell(RX, ry[0], ic16_bolt,   b);
    snprintf(b, sizeof(b), "%.0f C", d.st.vrTemp);    cell(LX, ry[1], ic16_chip,   b);
    snprintf(b, sizeof(b), "%d J/TH", effJTH(d.st));  cell(RX, ry[1], ic16_leaf,   b);
    snprintf(b, sizeof(b), "%d MHz", d.st.frequency); cell(LX, ry[2], ic16_wave,   b);
    cell(RX, ry[2], ic16_clock, fmtUptime(d.st.uptimeSeconds));

    // footer: the device's own config URL (web UI)
    display.setFont(NULL); display.setTextSize(1);
    String u = "cfg " + WiFi.localIP().toString();
    display.setCursor((display.width() - (int)u.length() * 6) / 2, 121);
    display.print(u);
  }, true);
}

// Fleet overview: total hashrate + per-device list (name, hashrate, temp).
void drawOverview() {
  renderFrame([&]() {
    const int W = display.width();
    char b[24];
    float total = 0; int rssi = 0;
    for (int i = 0; i < devCount; i++)
      if (devs[i].st.valid) { total += devs[i].st.hashRate; rssi = devs[i].st.rssi; }

    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(4, 18); display.print(fmtHash(total));
    display.setFont(&FreeSans9pt7b);
    snprintf(b, sizeof(b), "%d dev  %ddBm", devCount, rssi);
    display.setCursor(W - textW(b) - 4, 16); display.print(b);
    display.drawFastHLine(0, 22, W, GxEPD_BLACK);

    int y = 38;
    for (int i = 0; i < devCount && i < 6; i++) {
      const Dev& d = devs[i];
      String nm = d.st.hostname.length() ? d.st.hostname : String(d.ip);
      display.setCursor(6, y); display.print(fitW(nm, 130));
      if (d.st.valid) {
        char hh[12];
        if (d.st.hashRate >= 1000) snprintf(hh, sizeof(hh), "%.2fT", d.st.hashRate / 1000.0f);
        else                       snprintf(hh, sizeof(hh), "%.0fG", d.st.hashRate);
        display.setCursor(212 - textW(hh), y); display.print(hh);
        snprintf(b, sizeof(b), "%.0fC", d.st.temp);
        display.setCursor(W - textW(b) - 4, y); display.print(b);
      } else {
        display.setCursor(200, y); display.print("offline");
      }
      y += 15;
    }
  }, true);
}

// A.S.I.C. atom mark: 3 rotated orbit ellipses + a chip nucleus.
void drawAtom(int cx, int cy, int R) {
  for (int o = 0; o < 3; o++) {
    float ang = o * PI / 3.0f, ca = cosf(ang), sa = sinf(ang);
    float a = R, b = R * 0.42f;
    int lx = -9999, ly = 0;
    for (int t = 0; t <= 360; t += 10) {
      float r = t * PI / 180.0f;
      float ex = a * cosf(r), ey = b * sinf(r);
      int x = cx + (int)(ex * ca - ey * sa);
      int y = cy + (int)(ex * sa + ey * ca);
      if (lx != -9999) display.drawLine(lx, ly, x, y, GxEPD_BLACK);
      lx = x; ly = y;
    }
  }
  display.fillRect(cx - 4, cy - 4, 9, 9, GxEPD_BLACK);        // chip nucleus
  for (int i = -3; i <= 3; i += 3) {                          // chip legs
    display.drawPixel(cx + i, cy - 6, GxEPD_BLACK);
    display.drawPixel(cx + i, cy + 6, GxEPD_BLACK);
  }
}

// Setup / WiFi-config screen: atom logo + how to connect and configure.
void drawSetup() {
  renderFrame([&]() {
    drawAtom(40, 66, 28);
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(84, 20); display.print("A.S.I.C.");
    display.setFont(&FreeSans9pt7b);
    display.setCursor(84, 40); display.print("Miner Display");
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(84, 66); display.print("1. Join WiFi");
    display.setFont(&FreeSans9pt7b);
    display.setCursor(98, 84); display.print("paper-display");
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(84, 108); display.print("2. Open");
    display.setFont(&FreeSans9pt7b);
    display.setCursor(150, 108); display.print("192.168.4.1");
  }, true);
}

// ---- Bitaxe poll ---------------------------------------------------------
bool fetchStats(int idx) {
  Dev& d = devs[idx];
  if (WiFi.status() != WL_CONNECTED || strlen(d.ip) == 0) { d.st.valid = false; return false; }
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(5000);
  String url = "http://" + String(d.ip) + "/api/system/info";
  if (!http.begin(client, url)) { d.st.valid = false; return false; }
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, http.getString())) {
      d.st.hostname       = doc["hostname"].as<String>();
      d.st.hashRate       = doc["hashRate"].as<float>();
      d.st.temp           = doc["temp"].as<float>();
      d.st.vrTemp         = doc["vrTemp"].as<float>();
      d.st.power          = doc["power"].as<float>();
      d.st.frequency      = doc["frequency"].as<int>();
      d.st.sharesAccepted = doc["sharesAccepted"].as<long>();
      d.st.sharesRejected = doc["sharesRejected"].as<long>();
      d.st.bestDiff       = doc["bestDiff"].as<String>();
      d.st.uptimeSeconds  = doc["uptimeSeconds"].as<long>();
      d.st.rssi           = WiFi.RSSI();
      d.st.valid = true;
      pushSample(d, d.st.hashRate, d.st.temp);
      ok = true;
    }
  }
  if (!ok) d.st.valid = false;
  http.end();
  return ok;
}

// Poll every configured device; returns how many responded.
int pollAll() {
  int n = 0;
  for (int i = 0; i < devCount; i++) if (fetchStats(i)) n++;
  return n;
}

// Screen rotation, driven by RESPONDING devices only (so an offline or extra
// device never produces empty panels): overview (only when >1 responding) then
// each responding device's [graph][stats]. One device = just its two pages.
int screen = 0;
void drawCurrent() {
  if (devCount == 0) { drawMessage("No devices", "add IPs in", "paper-display AP"); return; }

  int valid[MAX_DEV], vc = 0;
  for (int i = 0; i < devCount; i++) if (devs[i].st.valid) valid[vc++] = i;

  if (vc == 0) {                       // nothing responding
    drawMessage("Offline", devCount == 1 ? devs[0].ip : "all devices", "check IP / power");
    return;
  }

  bool multi = vc > 1;
  int total = multi ? (1 + 2 * vc) : 2;
  int s = screen % total;
  if (multi && s == 0) { drawOverview(); return; }
  int idx = multi ? s - 1 : s;
  int dev = valid[idx / 2];
  int sub = idx % 2;
  if (sub == 0) drawGraphView(devs[dev]); else drawStatsView(devs[dev]);
}

// ---- web config UI -------------------------------------------------------
// A small HTTP server on the device's LAN IP (and http://paper-display.local/)
// to add/remove devices, see status, reconfigure WiFi, or reboot.
ESP8266WebServer server(80);
bool openPortalReq = false;

String webPage() {
  String h = F("<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>A.S.I.C. Miner Display</title><style>"
    "body{font-family:system-ui,sans-serif;background:#0b1d16;color:#eafff2;margin:0;padding:16px}"
    "h1{font-size:20px;color:#3ddc84;margin:.2em 0}"
    ".card{background:#0f2a1f;border:1px solid #1e7a4d;border-radius:10px;padding:14px;margin:12px 0}"
    "input[type=text]{width:100%;padding:9px;border-radius:6px;border:1px solid #1e7a4d;"
    "background:#06120d;color:#eafff2;box-sizing:border-box;font-size:15px}"
    "button{background:#3ddc84;color:#052915;border:0;border-radius:6px;padding:10px 14px;"
    "font-weight:700;margin-top:10px;cursor:pointer;font-size:15px}"
    ".b2{background:#1e7a4d;color:#eafff2}.muted{color:#7fd8a6;font-size:13px}"
    ".on{color:#3ddc84}.off{color:#ff8a8a}.row{margin:4px 0}</style></head><body>");
  h += F("<h1>&#9883; A.S.I.C. Miner Display</h1>");
  h += "<div class=card><b>WiFi:</b> " + WiFi.SSID() +
       "<br><span class=muted>" + WiFi.localIP().toString() + " &middot; " +
       String(WiFi.RSSI()) + " dBm</span></div>";
  h += F("<div class=card><b>Devices</b> <span class=muted>(up to 6)</span>");
  for (int i = 0; i < devCount; i++) {
    Dev& d = devs[i];
    h += "<div class=row>" + (d.st.hostname.length() ? d.st.hostname : String(d.ip)) + " &middot; ";
    if (d.st.valid) h += "<span class=on>" + fmtHash(d.st.hashRate) + " &middot; " +
                         String(d.st.temp, 0) + "C</span>";
    else            h += "<span class=off>offline</span>";
    h += " <span class=muted>(" + String(d.ip) + ")</span>";
    h += "<form method=POST action=/remove style=display:inline "
         "onsubmit=\"return confirm('Remove " + String(d.ip) + "?')\">"
         "<input type=hidden name=ip value='" + String(d.ip) + "'>"
         "<button class=b2 style='padding:2px 8px;margin-left:8px;font-size:12px'>&times;</button>"
         "</form></div>";
  }
  h += F("<form method=POST action=/add style='margin-top:12px'>"
         "<input type=text name=ip placeholder='add IP, e.g. 10.0.0.45'>"
         "<button type=submit>Add device</button></form></div>");
  h += F("<div class=card><b>WiFi settings</b><br>"
         "<span class=muted>Opens the setup hotspot to change WiFi.</span><br>"
         "<form method=POST action=/wifi onsubmit=\"return confirm('Open WiFi setup AP? "
         "This page drops until you reconnect.')\">"
         "<button class=b2 type=submit>Reconfigure WiFi</button></form>"
         "<form method=POST action=/reboot style=display:inline>"
         "<button class=b2 type=submit>Reboot</button></form></div>");
  h += F("</body></html>");
  return h;
}

void handleRoot()  { server.send(200, "text/html", webPage()); }
// Rebuild the persisted ipList string from the current device array.
void syncIpList() {
  String s = "";
  for (int i = 0; i < devCount; i++) { if (s.length()) s += ","; s += devs[i].ip; }
  strlcpy(ipList, s.c_str(), sizeof(ipList));
}

// Append one or more IPs to the device list WITHOUT disturbing existing
// devices (so their graph history is preserved). Skips dups / bad / over-limit.
void handleAdd() {
  if (server.hasArg("ip")) {
    String add = server.arg("ip"); add.trim();
    int start = 0;
    while (start <= (int)add.length()) {
      int c = add.indexOf(',', start);
      if (c < 0) c = add.length();
      String tok = add.substring(start, c); tok.trim();
      if (tok.length() && tok.indexOf('.') >= 0 && devCount < MAX_DEV) {
        bool dup = false;
        for (int i = 0; i < devCount; i++) if (String(devs[i].ip) == tok) dup = true;
        if (!dup) {
          Dev& d = devs[devCount];
          strlcpy(d.ip, tok.c_str(), sizeof(d.ip));
          d.histCount = 0; d.st = Stats();
          devCount++;
        }
      }
      start = c + 1;
    }
    syncIpList();
    saveConfig();
    screen = 0; pollAll();
  }
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "added");
  drawCurrent();
}

// Remove a device by IP, shifting the array in place (keeps others' history).
void handleRemove() {
  if (server.hasArg("ip")) {
    String target = server.arg("ip");
    int w = 0;
    for (int r = 0; r < devCount; r++) {
      if (String(devs[r].ip) == target) continue;     // drop the match
      if (w != r) devs[w] = devs[r];
      w++;
    }
    devCount = w;
    syncIpList();
    saveConfig();
    screen = 0; pollAll();
  }
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "removed");
  drawCurrent();
}
void handleWifi() {
  server.send(200, "text/html",
    "<meta http-equiv=refresh content='3;url=/'>Opening WiFi setup AP "
    "<b>paper-display</b> - join it to change WiFi.");
  openPortalReq = true;                 // handled in loop() (portal blocks)
}
void handleReboot() {
  server.send(200, "text/html", "<meta http-equiv=refresh content='6;url=/'>Rebooting...");
  delay(200); ESP.restart();
}
void setupWeb() {
  server.on("/",       HTTP_GET,  handleRoot);
  server.on("/add",    HTTP_POST, handleAdd);
  server.on("/remove", HTTP_POST, handleRemove);
  server.on("/wifi",   HTTP_POST, handleWifi);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.begin();
  if (MDNS.begin("paper-display")) MDNS.addService("http", "tcp", 80);
  Serial.printf("[paper-display] web UI: http://%s/  (http://paper-display.local/)\n",
                WiFi.localIP().toString().c_str());
}

// ---- setup / loop --------------------------------------------------------
void startPortal(bool onDemand) {
  WiFiManager wm;
  wm.setSaveConfigCallback(onSaveConfig);
  wm.setAPCallback([](WiFiManager*) { drawSetup(); });   // show setup screen when portal opens
  WiFiManagerParameter pIp("bitaxe", "Bitaxe IPs (comma-separated)", ipList, sizeof(ipList) - 1);
  wm.addParameter(&pIp);
  wm.setConfigPortalTimeout(180);

  bool connected = onDemand ? wm.startConfigPortal("paper-display")
                            : wm.autoConnect("paper-display");
  strlcpy(ipList, pIp.getValue(), sizeof(ipList));
  parseDevices(ipList);
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
  parseDevices(ipList);

  // No devices yet -> show the setup screen (also shown when the portal opens).
  if (devCount == 0) drawSetup();
  startPortal(false);                  // autoConnect: uses saved creds or portal

  Serial.printf("[paper-display] wifi=%s ip=%s devices=%d\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), devCount);

  setupWeb();                          // LAN config UI
  pollAll();
  drawCurrent();                       // first screen
}

// Serial config. To avoid boot/line noise ever corrupting the saved config, a
// command MUST be explicit: "ip 10.0.0.231,10.0.0.240" sets devices; "portal"
// opens WiFi setup. Anything else is ignored.
void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (line.equalsIgnoreCase("portal")) { startPortal(true); return; }
  if (!line.startsWith("ip ")) return;                 // ignore noise / other input
  String list = line.substring(3); list.trim();
  // sanity: only digits, dots, commas, spaces
  for (size_t i = 0; i < list.length(); i++) {
    char c = list[i];
    if (!((c >= '0' && c <= '9') || c == '.' || c == ',' || c == ' ')) return;
  }
  if (list.indexOf('.') < 0) return;
  strlcpy(ipList, list.c_str(), sizeof(ipList));
  parseDevices(ipList);
  saveConfig();
  Serial.printf("[paper-display] set %d device(s): %s\n", devCount, ipList);
  screen = 0; pollAll(); drawCurrent();
}

uint32_t lastPoll = 0;
void loop() {
  handleSerial();
  server.handleClient();
  MDNS.update();
  if (openPortalReq) {                  // /wifi requested: open portal (blocks)
    openPortalReq = false;
    startPortal(true);
    server.begin(); MDNS.begin("paper-display");   // re-listen after reconnect
    pollAll(); drawCurrent();
  }
  if (millis() - lastPoll >= POLL_MS || lastPoll == 0) {
    lastPoll = millis();
    int ok = pollAll();
    drawCurrent();
    screen++;                          // advance rotation each poll
    Serial.printf("[paper-display] polled %d/%d devices ok\n", ok, devCount);
  }
  delay(200);
}
