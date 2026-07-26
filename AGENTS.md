# Solar Beetle — Session Summary

## Objective
FireBeetle 2 ESP32-E battery monitor + string light controller with Wi-Fi, mDNS, HTMX dashboard, and physical RGB LED status.

## Hardware
- **Board**: DFRobot FireBeetle 2 ESP32-E (`dfrobot_firebeetle2_esp32e` in platformio.ini)
- **Battery ADC**: GPIO34, 2:1 divider (1 MΩ + 1 MΩ)
- **RGB LED**: WS2812 on GPIO5 (brightness 30)
- **Blue LED**: GPIO2, onboard
- **Red charge LED**: driven by TP4054 charger IC (not GPIO-controllable)
- **mDNS**: `solar.local` → `http://solar.local`
- **IP**: `192.168.1.224`
- **Wi-Fi / OTA credentials**: stored in `.env` (see `.env.sample`), loaded via `load_env.py` as compile-time `-D` flags

## Important — Battery Presence Detection
The onboard charger IC (TP4054) outputs voltage on the battery rail whenever USB is connected. GPIO34 always reads ~4.2 V on USB, **regardless of whether a battery is plugged in**. Battery presence **cannot** be detected through this ADC pin alone. No VBUS-sense pin is exposed on accessible GPIOs (GPIO36 does not work).

## LED Behavior

| LED | GPIO | Controllable | Behavior |
|---|---|---|---|
| Red charge | — (TP4054) | No (hardware auto) | On=charging, Off=charged, Flash=no battery |
| Blue | GPIO2 | Yes (on/off) | Solid on when ≥4.10 V (USB/charged), off on battery |
| RGB WS2812 | GPIO5 | Yes (full color) | Purple flash 80 ms every 3 s when WiFi + server active; off otherwise |

- The RGB purple flash replaces the old battery-voltage-based coloring and heartbeat.
- Battery status is still shown on the dashboard — the LEDs focus on power/connectivity at a glance.

## Dashboard
- HTML at `/`, HTMX polls `/status` every 10 s
- JSON at `/api/status` fields: `battery.voltage`, `battery.percent`, `battery.charged`, `battery.charging`, `battery.rate`, `light.mode`, `light.state`, `system.cpu`, `system.freeHeap`, `system.temp`, `system.rssi`, `system.uptime`, `firmware.hash`
- Battery SVG icon with animated fill, color-coded (green/yellow/red), bolt icon when charged
- Dashboard shows "Unplug at 80% for best life" hint when charged
- String light card with On/Off/Auto buttons, active button highlighted green
- Uptime card shows first 12 chars of firmware hash in subtitle
- Footer link to `/update` for OTA firmware upload

## String Light Control
- D10 = GPIO17, digital output
- Three modes: On (always HIGH), Off (always LOW), Auto (sunset-based)
- Buttons at `/api/light` GET/POST with `mode=on|off|auto`
- Auto mode uses self-contained NOAA-based sunrise/sunset algorithm
- Coordinates from `LAT`/`LON` in `.env` (Millbury, MA: 42.19, -71.76)
- Timezone: US/Eastern (`EST5EDT,M3.2.0,M11.1.0`)
- NTP sync at boot via `configTzTime()`
- Sunrise/sunset recalculated every 12 hours
- **Battery power management** in Auto mode: turns lights off when voltage < 3.40 V to protect battery
- **Cloudy-day detection**: tracks voltage trend during daylight; if voltage rises < 0.05 V from sunrise by 10 min past sunrise, marks as cloudy and conserves power
- **Critical protection**: all modes forced off when voltage < 3.30 V
- Dashboard shows "Battery low — conserving" or "Cloudy — conserving" when power saving is active
- JSON fields: `light.powerSaving`, `light.cloudy`

## Firmware Identity
- **MD5 hash**: reported at `firmware.hash` in `/api/status`, also shown (first 12 chars) in dashboard Uptime card subtitle
- **Version**: use `git describe --tags --always --dirty` — no formal version file; git commit is the source of truth
- Verify running firmware matches build: `md5sum .pio/build/ota/firmware.bin` vs `/api/status`

## OTA Updates
- **Dual A/B OTA** via custom `partitions.csv`: two 1.75 MB app slots, 448 KB SPIFFS
- **ArduinoOTA** (hostname from `OTA_HOSTNAME` env var) for CLI-based flashing (`pio run --target upload --upload-port solar.local`)
- **Web upload** at `/update`: GET shows form, POST receives .bin and flashes inactive slot
- Rollback: `esp_ota_mark_app_valid_cancel_rollback()` called on every boot to confirm firmware is good

## Power Conservation (board-wide)
When battery voltage drops, the board reduces its own draw to stay alive longer:
- **Bluetooth disabled at boot** — saves ~10 mA (BT never used)
- **≥ 3.50 V** (normal): CPU at 240 MHz, full Wi-Fi TX power (19.5 dBm)
- **< 3.40 V** (low): CPU drops to 160 MHz, Wi-Fi TX power drops to 8.5 dBm (saves ~60 mA during bursts, safe within 10 m of router)
- 160 MHz keeps APB at 80 MHz so NeoPixel RMT timing is unaffected
- Power mode checked every 10 s with hysteresis (recovers at 3.50 V)

## Battery Health
- Best longevity at 20–80 % charge (3.40 – 3.90 V)
- Charging to 100 % (4.20 V) stresses the cell
- Discharging below 20 % (3.30 V) stresses the cell

## Project Structure
- `src/main.cpp` — firmware: ADC, web server, mDNS, NeoPixel, all endpoints
- `data/index.html` — HTMX dashboard
- `platformio.ini` — board + deps + extra_scripts
- `load_env.py` — pre-build script: reads `.env` and generates `include/credentials.h`
- `include/credentials.h` — auto-generated from `.env` (not committed)
- `.env` — local credentials (not committed; create from `.env.sample`)
- `.env.sample` — template with placeholder values (committed)
- `keys/private.pem` — RSA-2048 private key for signing firmware (not committed)
- `include/public_key.h` — embedded RSA-2048 public key (committed)
- `.venv/` — uv virtualenv with PlatformIO

## Setup
```bash
cp .env.sample .env        # first time — fill in your credentials
```

## Build & Flash

### OTA (primary — device must be running)
```bash
source .venv/bin/activate
pio run --environment ota                    # build
pio run --environment ota --target upload    # flash firmware OTA
pio run --environment ota --target uploadfs  # flash SPIFFS (HTML) OTA
```

### USB (fallback — connect via USB, press boot+reset)
```bash
source .venv/bin/activate
pio run                                      # build
pio run --target upload                      # flash firmware via USB
pio run --target uploadfs                    # flash SPIFFS (HTML) via USB
```

## Dependencies (platformio.ini)
- `ArduinoJson` (JSON endpoint)
- `Adafruit NeoPixel` (RGB LED)

## .env Variables
| Key | Required | Default |
|---|---|---|
| `WIFI_SSID` | Yes | — |
| `WIFI_PASSWORD` | Yes | — |
| `OTA_HOSTNAME` | No | `solar` |
| `OTA_PASSWORD` | Yes | — |
| `MDNS_HOSTNAME` | No | `solar` |
| `LAT` | Yes | `42.19` |
| `LON` | Yes | `-71.76` |
| `TZ` | No | `EST5EDT,M3.2.0,M11.1.0` |
| `LIGHTS_OFF_TIME` | No | `1.0` |
