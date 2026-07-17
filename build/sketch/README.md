#line 1 "C:\\repos\\esp32-atchome\\README.md"
# esp32-atchome
ESP32 Home Air Traffic Control

## Hardware

CrowPanel ESP32 HMI Display Module (7") P/N: DIS08070H
- Display: ILI9488 800x480 TFT (SPI interface)
- Touch: FT6236 Capacitive Touchscreen (I2C interface)

## Test Project: Display and Touch Demo

This minimal test project demonstrates successful programming of the ESP32 with display and touch functionality.

### Features
- Initializes LVGL graphics library
- Displays a red rectangle on the 7" screen
- Responds to capacitive touch input
- Moves rectangle to random location when tapped

### Quick Start

**Prerequisites:**
- Arduino CLI v1.5+ installed
- ESP32 board support installed
- LVGL 9.5.0 library installed
- FT6236G touch driver library installed
- USB cable connected to ESP32 (COM port identified)

**Build:**
```bash
# Using Arduino CLI directly
arduino-cli compile --fqbn esp32:esp32:esp32 .

# Using Makefile (Windows)
make compile
```

**Upload:**
```bash
# Using Arduino CLI (replace COM3 with your port)
arduino-cli upload --fqbn esp32:esp32:esp32 --port COM3 --speed 921600 .

# Using Makefile
PORT=COM3 make upload

# Or just
make all  # Compiles and uploads
```

**Verify Execution:**
1. After upload completes, open serial monitor at 115200 baud
2. You should see: "Setup complete. Display and touch initialized."
3. A red rectangle appears on the display
4. Touch the rectangle to move it to a random location
5. Serial output confirms each touch event

### Pin Configuration

**Display (ILI9488 via SPI):**
- GPIO 18: CLK
- GPIO 23: MOSI
- GPIO 19: MISO
- GPIO 5: CS
- GPIO 2: DC
- GPIO 4: RST

**Touch (FT6236 via I2C):**
- GPIO 21: SDA
- GPIO 22: SCL
- I2C Address: 0x38

### Project Structure
```
esp32-atchome/
├── esp32-atchome.ino      # Main sketch (entry point)
├── src/                   # Source code
│   └── main.cpp          # Display and touch logic
├── lib/                   # Libraries
│   └── lv_conf.h         # LVGL configuration
├── build/                # Compiled output
├── arduino-cli.yaml      # Arduino CLI configuration
├── Makefile             # Build automation
└── README.md            # This file
```

### Troubleshooting

**"main file missing" error:**
- Ensure esp32-atchome.ino exists in the project root

**"lv_conf.h not found" error:**
- Copy lib/lv_conf.h to Arduino/libraries/ directory

**Upload fails:**
- Check COM port: `arduino-cli board list`
- Verify USB driver installed for ESP32
- Try slower baud rate: `--speed 115200`

**Touch not working:**
- Verify I2C address (0x38) is correct for your FT6236 variant
- Check SDA/SCL pin connections
- Enable I2C debug output in serial monitor
