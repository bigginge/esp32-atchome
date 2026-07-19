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

- **Radar** (480×480): concentric range rings centred on your location; aircraft symbols oriented by track with altitude-aware colour and trails (lower altitude = more distinct trails)
- **Info panel** (320×480): manufacturer, type, registration, flight number, distance (nm), origin and destination airports
- **Touch select**: tap an aircraft to highlight it and load details; selection clears when it leaves range; defaults to the nearest aircraft
- **Data**: [adsb.fi](https://adsb.fi/) positions; [hexdb.io](https://hexdb.io/) aircraft and route enrichment (lazy, for the selection)

## Prerequisites

- Arduino CLI 1.5+
- ESP32 board package: `arduino-cli core install esp32:esp32`
- Libraries:

```bash
arduino-cli lib install "lvgl" "LovyanGFX" "PCA9557-arduino" "ArduinoJson"
```

Board index (see `arduino-cli.yaml`):

`https://espressif.github.io/arduino-esp32/package_esp32_index.json`

## Configuration

```bash
cp config.example.h config.h
```

Edit `config.h`: WiFi SSID/password, latitude/longitude, search radius, refresh interval.  
`config.h` is gitignored.

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
├── config.h                 # local, gitignored
├── lv_conf.h
├── src/
│   ├── main.cpp
│   ├── crowpanel_display.hpp
│   ├── aircraft.hpp
│   ├── geo.hpp
│   ├── api_client.cpp/.hpp
│   ├── tracker.cpp/.hpp
│   ├── radar_view.cpp/.hpp
│   └── info_panel.cpp/.hpp
├── Makefile
└── README.md
```

## Serial

115200 baud. Expect WiFi connect, ADS-B fetch logs, then a live radar display.
