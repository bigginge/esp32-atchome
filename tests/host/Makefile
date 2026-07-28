# Host-side tests for the radar render path. Not part of the firmware build --
# `make compile` in the project root never comes here.
#
#   make          fetch LVGL if needed, build and run
#   make clean    remove the binary and the fetched LVGL
#
# LVGL is fetched rather than vendored: the firmware takes whatever
# `arduino-cli lib install lvgl` gave you, so pinning a copy here would only
# create a second version to disagree with. Override to match your install:
#
#   make LVGL_TAG=v9.3.0

LVGL_TAG ?= v9.5.0
LVGL_DIR ?= .lvgl-$(LVGL_TAG)
ROOT     := ../..

CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -Wno-unused-parameter \
            -DLV_CONF_INCLUDE_SIMPLE \
            -I stub -I $(ROOT) -I $(ROOT)/src -I . -I $(LVGL_DIR) -I $(LVGL_DIR)/src

# radar_view.cpp is compiled, not linked against, so the tests can reach the
# geometry directly. That needs its private members, hence the override; it is
# confined to this build and never reaches the firmware.
CXXFLAGS += -Dprivate=public

SRCS := test_sweep.cpp lv_stub.cpp $(ROOT)/src/radar_view.cpp

.PHONY: all run clean

all: run

$(LVGL_DIR):
	git clone --depth 1 --branch $(LVGL_TAG) https://github.com/lvgl/lvgl.git $@

test_sweep: $(LVGL_DIR) $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ -lm

run: test_sweep
	./test_sweep

clean:
	rm -rf test_sweep $(LVGL_DIR)
