# Solar Beetle

An ESP32 battery monitor and string-light controller for a small solar setup.
Built on a DFRobot FireBeetle 2 ESP32-E, it reports battery and system metrics
over Wi-Fi, serves a live HTMX dashboard, controls outdoor string lights on a
sunrise/sunset schedule, and supports signed over-the-air (OTA) firmware updates.

- **Board:** DFRobot FireBeetle 2 ESP32-E
- **Firmware:** Arduino framework, built with [PlatformIO](https://platformio.org/)
- **Dashboard:** server-rendered HTML + HTMX (no build step), stored on SPIFFS
- **Security:** web OTA requires RSA-2048 / SHA-256 signed firmware images

---

## Features

- **Battery monitoring** — ADC voltage reads through a 2:1 divider on GPIO34,
  reported as volts, percent, and charge state.
- **Live dashboard** — `http://solar.local` polls `/status` every 10 s;
  battery icon, system metrics, and firmware hash update in place via HTMX.
- **JSON API** — `/api/status` exposes the same data for external consumers.
- **String-light control** — GPIO17 (D10) with `On` / `Off` / `Auto` modes.
  Auto mode uses a self-contained NOAA sunrise/sunset algorithm and coordinates
  from `.env`.
- **Power conservation** —
  - Bluetooth is disabled at boot (~10 mA saved).
  - Below 3.40 V the CPU drops to 160 MHz and Wi-Fi TX power to 8.5 dBm; it
    recovers at 3.50 V (hysteresis).
  - In Auto mode lights switch off below 3.40 V, and a cloudy-day voltage-trend
    check conserves power when solar isn't keeping up. Critical cutoff at 3.30 V.
- **Status LEDs** —
  - Onboard blue LED (GPIO2): solid on when charged / on USB (≥4.05 V).
  - WS2812 RGB (GPIO5): purple flash every 3 s when Wi-Fi + server are live.
  - Red charge LED is driven by the TP4054 charger IC (not GPIO-controllable).
- **Signed OTA** — firmware uploaded through the web `/update` page must carry a
  256-byte RSA-2048 SHA-256 signature appended to the `.bin`; unsigned uploads
  are rejected. Dual A/B partition slots allow safe rollback.

---

## Hardware

| Function | Pin | Notes |
|---|---|---|
| Battery ADC | GPIO34 | 2:1 divider (1 MΩ + 1 MΩ) |
| RGB LED (WS2812) | GPIO5 | brightness 30 |
| Blue LED | GPIO2 | onboard |
| String lights | GPIO17 (D10) | digital output |
| Red charge LED | — | TP4054 hardware, auto |

> **Battery presence cannot be detected from the ADC.** The TP4054 charger IC
> energizes the battery rail whenever USB is connected, so GPIO34 reads ~4.2 V
> with or without a battery plugged in.

---

## Project Layout

```
solar-beetle/
├── src/main.cpp          # firmware: ADC, web server, mDNS, NeoPixel, OTA, signing
├── data/index.html       # HTMX dashboard (flashed to SPIFFS)
├── include/
│   ├── public_key.h      # embedded RSA-2048 public key (committed)
│   └── credentials.h     # auto-generated from .env — DO NOT COMMIT
├── keys/
│   ├── private.pem       # RSA-2048 private key — DO NOT COMMIT
│   └── public.der        # public key in DER form (committed)
├── tools/sign_firmware.sh# sign a build for web OTA
├── load_env.py           # pre-build script: .env → include/credentials.h
├── partitions.csv        # dual A/B OTA partition table
├── platformio.ini        # build config (USB + OTA environments)
└── .env.sample           # credential template
```

---

## Setup

### 1. Install tooling

Use the bundled `uv` virtualenv, or install PlatformIO globally:

```bash
# Option A — bundled venv (recommended)
source .venv/bin/activate

# Option B — global
pip install platformio
```

### 2. Configure credentials

```bash
cp .env.sample .env
```

Edit `.env` and fill in your values:

| Key | Required | Default | Description |
|---|---|---|---|
| `WIFI_SSID` | Yes | — | Wi-Fi network name |
| `WIFI_PASSWORD` | Yes | — | Wi-Fi password |
| `OTA_HOSTNAME` | No | `solar` | ArduinoOTA hostname |
| `OTA_PASSWORD` | Yes | — | OTA update password |
| `MDNS_HOSTNAME` | No | `solar` | mDNS hostname (`http://<name>.local`) |
| `LAT` | Yes | `42.19` | Latitude for sunrise/sunset |
| `LON` | Yes | `-71.76` | Longitude for sunrise/sunset |

`load_env.py` runs automatically before each build and writes
`include/credentials.h` from `.env`. That file is gitignored.

---

## Build & Flash

There are two PlatformIO environments:

- **`firebeetle2-esp32e`** — USB upload (fallback).
- **`ota`** — uploads over Wi-Fi to `solar.local` via espota.

### USB (first flash / fallback)

```bash
pio run -e firebeetle2-esp32e            # build
pio run -e firebeetle2-esp32e --target upload    # flash firmware
pio run -e firebeetle2-esp32e --target uploadfs  # flash dashboard (SPIFFS)
pio device monitor                       # serial @ 115200
```

### OTA (subsequent flashes)

```bash
pio run -e ota                           # build
pio run -e ota --target upload           # flash firmware over Wi-Fi
pio run -e ota --target uploadfs         # flash dashboard over Wi-Fi
```

The device is reachable at `http://solar.local` (or its IP from the serial
monitor) once booted and connected.

---

## Usage

1. Configure `.env` and flash the firmware + filesystem.
2. Open `http://solar.local` in a browser — the dashboard polls every 10 s.
3. Use the **String Lights** card to switch `On` / `Off` / `Auto`.
4. Check **Firmware** to confirm the running MD5 matches your build:

   ```bash
   md5sum .pio/build/ota/firmware.bin
   # compare with the hash shown on the dashboard / in /api/status
   ```

---

## Endpoints

| Path | Content-Type | Description |
|---|---|---|
| `/` | `text/html` | HTMX dashboard (from SPIFFS) |
| `/status` | `text/html` | HTMX fragment: battery + system metrics |
| `/api/status` | `application/json` | Full status (see below) |
| `/api/light` | `text/html` | `GET`/`POST` `?mode=on\|off\|auto` to set light mode |
| `/update` | `text/html` | Firmware upload form (`GET`) / OTA flash (`POST`, signed only) |

### `/api/status` fields

```json
{
  "battery": { "voltage": 4.12, "percent": 100, "charged": true },
  "light":   { "mode": "auto", "state": false, "powerSaving": false, "cloudy": false },
  "system":  { "cpu": 240, "freeHeap": 123456, "temp": 42.0,
               "rssi": -55, "uptime": 12345 },
  "firmware": { "hash": "abcdef0123..." }
}
```

---

## Firmware Signing & OTA

Web uploads through `/update` only accept **signed** images. The signature is a
256-byte RSA-2048 PKCS#1 v1.5 SHA-256 blob appended to the firmware `.bin` and
verified against the public key embedded in `include/public_key.h`.

```bash
# Build, then sign the binary
pio run -e ota
./tools/sign_firmware.sh .pio/build/ota/firmware.bin firmware.signed.bin

# Upload firmware.signed.bin at http://solar.local/update
```

CLI uploads via `pio run -e ota --target upload` use ArduinoOTA's own password
auth (`OTA_PASSWORD`) and do not require signing.

A bad web upload fails signature verification and leaves the previous firmware
running. The device calls `esp_ota_mark_app_valid_cancel_rollback()` on every
boot, so a wedged OTA slot doesn't trigger automatic rollback.

### Regenerating keys (optional)

```bash
openssl genrsa -out keys/private.pem 2048
openssl rsa -in keys/private.pem -pubout -out keys/public.der -outform DER
# Re-embed the public key into include/public_key.h (e.g. with xxd -i)
```

Keep `keys/private.pem` secret and gitignored.

---

## Partition Layout

Defined in `partitions.csv` — two 1.75 MB application slots plus 448 KB SPIFFS:

| Name | Offset | Size |
|---|---|---|
| nvs | 0x9000 | 20 KB |
| otadata | 0xE000 | 8 KB |
| app0 | 0x10000 | 1.75 MB |
| app1 | 0x1D0000 | 1.75 MB |
| spiffs | 0x390000 | 448 KB |

---

## Battery Guidance

- Keep charge between **20–80 %** (3.40–3.90 V) for best cell longevity.
- Charging to 100 % (4.20 V) and discharging below 20 % (3.30 V) stress the cell.
- The firmware enforces protective cutoffs (lights off < 3.40 V; everything off
  < 3.30 V) and throttles its own draw when the battery is low.

| Voltage | Status |
|---|---|
| 4.05 – 4.20 V | Full / Charged |
| 3.70 – 4.04 V | Nominal |
| 3.40 – 3.69 V | Low |
| < 3.30 V | Critical |

---

## Development

- **`.env`** holds secrets; never commit it. `include/credentials.h` is generated
  and gitignored.
- **`AGENTS.md`** contains the full session/design notes used during
  development — useful background but not required to build.
- **CI** builds both environments on every push/PR via GitHub Actions
  (`.github/workflows/build.yml`).

## License

[MIT](LICENSE) © 2026 bdragoncore
