# paper-display

Live **Bitaxe (AxeOS)** stats on a **2.9″ black/white e-paper** display, driven by
an **ESP8266**. A low-power, always-on companion screen for your miner — inspired by
[CYD-External-Display-for-Bitaxe](https://github.com/ShaeOJ/CYD-External-Display-for-Bitaxe),
rebuilt for e-paper.

![panel](docs/panel.jpg)

## What it shows
Polls `http://<bitaxe-ip>/api/system/info` every 30 s and renders:

- **Hashrate** (GH/s, auto-switches to TH/s)
- ASIC **temp** + **VR temp**
- **Power** (W) and computed **efficiency** (J/TH)
- **Frequency** (MHz)
- **Shares** accepted / rejected
- **Best difficulty**
- Uptime + WiFi signal

## Hardware
- **Waveshare e-Paper ESP8266 Driver Board** (ESP-12F, 4 MB, CP2102)
- **2.9″ 296×128 B/W e-paper** panel (tested with the Heltec Meshtastic 2.9″ panel)

Driver-board pin map (fixed by the PCB):

| Signal | GPIO |
|--------|------|
| BUSY   | 16   |
| RST    | 5    |
| DC     | 4    |
| CS     | 15   |
| SCK    | 14   |
| MOSI   | 13   |

## Build & flash (PlatformIO)
```bash
pio run -t upload        # board=esp12e, uploads to COM7
pio device monitor       # 115200
```

## First-run setup
On first boot (or if WiFi fails) it opens a captive-portal AP named **`paper-display`**:

1. Join the `paper-display` WiFi network from your phone/PC.
2. Pick your WiFi and enter the password.
3. Enter your **Bitaxe IP** (e.g. `10.0.0.42`) in the extra field.
4. Save — settings persist in LittleFS; it reconnects automatically after.

## Notes
- Full-window refresh every 30 s (no ghosting). e-paper only redraws on the poll
  cycle, so it sips power between updates.
- Panel class is `GxEPD2_290_BS`. If you use a different 2.9″ panel and see a slow
  ~10 s *Busy Timeout* per refresh, switch the class in `src/main.cpp` to
  `GxEPD2_290_T94_V2` (GDEM029T94) and re-flash.

## Libraries
GxEPD2 · ArduinoJson v6 · WiFiManager (tzapu)
