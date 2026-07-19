.PHONY: compile upload verify clean help all

FQBN = esp32:esp32:esp32s3:PSRAM=opi,FlashMode=qio,FlashSize=4M,PartitionScheme=huge_app,CDCOnBoot=cdc,USBMode=hwcdc
PORT ?= COM3
BAUD = 921600
SKETCH_DIR := $(CURDIR)

help:
	@echo "ESP32 ATC Home — build commands"
	@echo "  make compile          Compile sketch"
	@echo "  make upload           Upload to board"
	@echo "  make all              Compile and upload"
	@echo "  make clean            Remove build/"
	@echo "  make PORT=COMx upload Change serial port"

compile:
	arduino-cli compile --fqbn $(FQBN) --build-property "compiler.cpp.extra_flags=-I$(SKETCH_DIR) -DLV_CONF_INCLUDE_SIMPLE" $(SKETCH_DIR) --verbose

upload:
	arduino-cli upload --fqbn $(FQBN) --port $(PORT) --upload-property upload.speed=$(BAUD) $(SKETCH_DIR) --verbose

verify: compile
	@echo "Sketch compiled successfully"

clean:
	rmdir /S /Q build 2>nul || true

all: compile upload
	@echo "Build and upload complete"
