.PHONY: compile upload verify clean help

FQBN = esp32:esp32:esp32s3
PORT ?= COM3
BAUD = 921600

help:
	@echo "ESP32 Display and Touch Demo - Build Commands"
	@echo "=============================================="
	@echo "make compile    - Compile the sketch"
	@echo "make upload     - Upload to ESP32"
	@echo "make verify     - Compile and verify"
	@echo "make clean      - Clean build artifacts"
	@echo "make all        - Compile and upload"
	@echo ""
	@echo "Optional: PORT=COMx make upload   (change upload port)"

compile:
	arduino-cli compile --fqbn $(FQBN) .

upload:
	arduino-cli upload --fqbn $(FQBN) --port $(PORT) --speed $(BAUD) .

verify: compile
	@echo "Sketch compiled successfully"

clean:
	rmdir /S /Q build 2>nul || true

all: compile upload
	@echo "Build and upload complete"

