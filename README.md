# esp32-atchome
ESP32 Home Air Traffic Control

## Hardware

CrowPanel ESP32-S3 HMI Display Module (7") P/N: DIS08070H
- **Microcontroller**: ESP32-S3 (Dual Core, 240MHz, 8MB PSRAM)
- **Display**: ILI9488 800x480 TFT (SPI interface)
- **Touch**: FT6236G Capacitive Touchscreen (I2C interface)

## Test Project: Display and Touch Demo

This minimal test project demonstrates successful programming of the ESP32-S3 with display and touch functionality.

### Current Status
✅ **Successfully uploaded and running on ESP32-S3 (COM3)**
- Sketch compiles and uploads without errors
- I2C bus scanner detects connected I2C devices (FT6236 at address 0x38 when connected)
- Serial output confirmed via 115200 baud monitor

### Features
- Initializes I2C for touch controller detection
- Scans I2C bus every 5 seconds for connected devices
- Reports device addresses (ready for display/touch integration)
- Foundation for LVGL graphics and touch handling

### Quick Start

**Prerequisites:**
- Arduino CLI v1.5+ installed
- ESP32 board support installed (3.3.10+)
- USB cable connected to ESP32-S3 (COM3 or your port)

**Build:**
```bash
# Using Arduino CLI directly
arduino-cli compile --fqbn esp32:esp32:esp32s3 .

# Using Makefile (Windows)
make compile
```

**Upload:**
```bash
# Using Arduino CLI (replace COM3 with your port)
arduino-cli upload --fqbn esp32:esp32:esp32s3 --port COM3 .

# Using Makefile
PORT=COM3 make upload

# Or just
make all  # Compiles and uploads
```

**Verify Execution:**
1. After upload completes, open serial monitor at 115200 baud
2. You should see: "===== ESP32 Display and Touch Demo ====="
3. You should see: "I2C initialized successfully!"
4. Every 5 seconds, you will see "Scanning I2C bus..." followed by device detection
5. When FT6236 is connected, you should see: "I2C device found at address 0x38"

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

**Upload fails with "Wrong chip argument":**
- Device is ESP32-S3, not regular ESP32
- Use FQBN: `esp32:esp32:esp32s3` (already configured in arduino-cli.yaml)

**No serial output:**
- Verify COM port is correct: `arduino-cli board list`
- Ensure baud rate is 115200
- Try holding RESET button after upload

**No I2C devices found:**
- FT6236 touch controller may not be connected
- Verify I2C connections (GPIO 21 SDA, GPIO 22 SCL)
- Check I2C address with external I2C scanner

### Next Steps: Full Display and Touch Demo

The current sketch is a simplified I2C bus scanner. To add full LVGL graphics and touch support:

1. **Resolve LVGL Version**: 
   - LVGL 9.5.0 is installed but uses different API than the original sketch (designed for LVGL 8)
   - Option A: Downgrade to LVGL 8.3.11: `arduino-cli lib install "lvgl:8.3.11"`
   - Option B: Rewrite sketch for LVGL 9 API (more complex, more modern)
   - Option C: Use alternative TFT driver (TFT_eSPI, etc.)

2. **Implement Display Driver**: Initialize ILI9488 over SPI with custom flush callback

3. **Implement Touch Driver**: Add FT6236 touch event handling with LVGL integration

4. **Graphics Demo**: Create LVGL UI with rectangle that responds to touch events

**Current Files:**
- `esp32-atchome.ino`: Active simplified I2C demo
- `src/main.cpp.bak`: Original LVGL 8 design (requires LVGL 9 API updates to compile)
