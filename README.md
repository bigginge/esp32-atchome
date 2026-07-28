# esp32-atchome

ESP32 Home Air Traffic Control for the CrowPanel ESP32-S3 7" display.

Live ADS-B traffic on a square radar (left) with an aircraft detail panel (right).

## Hardware

**CrowPanel ESP32-S3 HMI 7" (DIS08070H)**

| Part | Detail |
| --- | --- |
| MCU | ESP32-S3-WROOM-1-N4R8 (OPI PSRAM) |
| Display | 800×480 RGB TFT |
| Touch | GT911 capacitive, I2C SDA **19** / SCL **20** |
| Touch reset | PCA9557 at **0x18** |
| Backlight | GPIO **2** |

## Features

- **Radar** (480×480): concentric range rings (labelled in nm) and N/E/S/W compass markers centred on your location; plane-shaped aircraft symbols oriented by track, coloured by altitude (green-cyan low → magenta high) with an altitude legend, per-aircraft callsign + flight-level labels, and trails (lower altitude = more distinct trails)
- **Sweep beam**: a PPI beam rotates once every 6 s, with a phosphor tail behind it and a flare on each aircraft as it is crossed. It is composited by hand straight into the framebuffer — LVGL has no primitive for a wedge that would not cost a mask per pixel — and only the swept wedge is repainted, so it costs about a fifth of one core rather than a full-screen redraw per frame. The dials are at the top of `src/radar_view.hpp`, with the cost model that relates them; `kSweepEnabled = false` turns it off
- **Info panel** (320×480): live clock, your location, aircraft-in-range count and data-freshness ("updated Ns ago"); for the selection: manufacturer, type, registration, flight number, distance (nm), altitude, ground speed, and origin/destination airports
- **Responsive UI**: all network requests run on the ESP32-S3's second core, so the radar animates smoothly and touch stays responsive while data is fetched
- **Touch select**: tap an aircraft to highlight it and load details; selection clears when it leaves range; defaults to the nearest aircraft
- **Data**: [adsb.fi](https://adsb.fi/) positions (polled every 5 s); [hexdb.io](https://hexdb.io/) aircraft and route enrichment (lazy, for the selection)
- **On-device setup**: pick your WiFi network and type the password on the touchscreen, and set your location (optionally prefilled from IP geolocation). Nothing is hardcoded; settings live in NVS and are reachable any time from the gear button on the info panel

## Prerequisites

- Arduino CLI 1.5+
- ESP32 board package: `arduino-cli core install esp32:esp32`
- Libraries:

```bash
arduino-cli lib install "lvgl" "PCA9557-arduino" "ArduinoJson"
```

The display uses the ESP-IDF `esp_lcd` RGB driver (bundled with the ESP32 core) with two PSRAM
framebuffers and VSYNC page-flip for tear-free output; touch is a small built-in GT911 reader. No
external graphics library is required.

Board index (see `arduino-cli.yaml`):

`https://espressif.github.io/arduino-esp32/package_esp32_index.json`

## Configuration

Nothing needs configuring at build time. Flash the sketch and the device walks you
through setup on its own screen:

1. **WiFi** — pick your network from the scan list (or *Other network...* for a hidden
   SSID) and type the password on the on-screen keyboard.
2. **Location** — the fields are prefilled from your saved settings, with a suggestion
   derived from your public IP shown above them. Tap **Use detected** to accept it, or
   type your own coordinates. IP geolocation is city-level at best and can be far out;
   since the radar draws the whole search radius across 480 px, it is worth entering
   real coordinates ([latlong.net](https://www.latlong.net/)) if the suggestion looks off.

Settings are stored in NVS and survive reflashing. Tap the **gear** in the top-right of
the info panel to change them later — including moving the device to a different network.

### Optional: seed the settings at build time

```bash
cp config.example.h config.h
```

`config.h` (gitignored) is a convenience for pre-filling a device you flash often. Its
values are written to NVS **once, on first boot**, and if it supplies a WiFi network the
wizard is skipped entirely.

> **After that first boot, `config.h` is inert.** Editing it and reflashing changes
> nothing, because NVS already holds the settings. Change things from the wizard, or
> wipe NVS with `esptool erase_flash` to seed again.

### Notes

- WPA/TKIP-only access points are refused: the ESP32 core's default minimum security is
  `WIFI_AUTH_WPA2_PSK`. Open networks and WPA2/WPA3 work.
- The IP-geolocation lookup ([ip-api.com](https://ip-api.com/)) is plain HTTP — its free
  tier has no TLS endpoint. It is only ever a suggestion you confirm on screen, and the
  coordinates are range-checked before being offered. Everything else the device fetches
  goes over HTTPS.

## Build / upload

```powershell
$fqbn = "esp32:esp32:esp32s3:PSRAM=opi,FlashMode=qio,FlashSize=4M,PartitionScheme=huge_app,CDCOnBoot=cdc,USBMode=hwcdc"
arduino-cli compile --fqbn $fqbn --build-property "compiler.cpp.extra_flags=-I$PWD -DLV_CONF_INCLUDE_SIMPLE" .
arduino-cli upload --fqbn $fqbn --port COM3 --upload-property upload.speed=921600 .
```

Or: `make compile` / `make PORT=COM3 upload`

## Project layout

```
esp32-atchome/
├── esp32-atchome.ino
├── config.example.h
├── config.h                 # optional first-boot seed, gitignored
├── lv_conf.h
├── src/
│   ├── main.cpp
│   ├── crowpanel_display.hpp
│   ├── aircraft.hpp
│   ├── geo.hpp
│   ├── text_util.hpp
│   ├── api_client.cpp/.hpp
│   ├── geo_ip.cpp/.hpp       # IP geolocation (plain HTTP; suggestion only)
│   ├── settings.cpp/.hpp     # NVS-backed runtime config
│   ├── net_control.cpp/.hpp  # UI <-> network-task command channel
│   ├── settings_screen.cpp/.hpp
│   ├── tracker.cpp/.hpp
│   ├── radar_view.cpp/.hpp
│   └── info_panel.cpp/.hpp
├── tests/host/              # workstation checks for the sweep geometry
├── Makefile
├── PERFORMANCE.md           # standing review of where the frame budget goes
└── README.md
```

## Performance

`src/frame_probe.hpp` records the measurement history — what each stage of the
render path costs, and the two traps that show up as a collapsed `loop` rate
rather than as a slow function. [PERFORMANCE.md](PERFORMANCE.md) is the
forward-looking half: a ranked plan for what to do next, why the largest item
is a build-time default rather than anything in `src/`, and which plausible
optimisations were checked and ruled out.

## Tests

```bash
make -C tests/host
```

Builds `src/radar_view.cpp` against real LVGL headers on the workstation (fetched
on first run; `make -C tests/host LVGL_TAG=v9.3.0` to match a different install)
and checks the sweep beam's geometry against a brute-force reference — that the
sector solver returns exactly the pixels an atan2 would classify, that the bands
handed to LVGL cover the whole swept wedge, and that the composite writes every
pixel of the region it is given. That last one is the one that matters: with
double-buffered DIRECT rendering, a pixel inside an invalid area that nobody
writes keeps content from two frames ago, so it shows as a dot the beam left
behind and never clears.

Nothing here touches the firmware build; `make compile` never enters `tests/`.

## Serial

115200 baud. Expect WiFi connect, ADS-B fetch logs, then a live radar display.
