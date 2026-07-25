#pragma once

#include <stdint.h>

/** Minimal GT911 capacitive-touch reader over the Arduino Wire bus.
 *  Assumes Wire.begin(SDA, SCL) was already called (the PCA9557 reset
 *  sequence in main.cpp does this) and that the controller sits at 0x5D. */
class GT911 {
 public:
  explicit GT911(uint8_t address = 0x5D) : addr_(address) {}

  /** Read the primary touch point. Returns true while a finger is down and
   *  writes screen coordinates to *x/*y. Holds the last point briefly across
   *  not-ready polls so a press reads as stable. */
  bool read(uint16_t *x, uint16_t *y);

 private:
  uint8_t addr_;
  uint16_t lastX_ = 0;
  uint16_t lastY_ = 0;
  uint32_t lastTouchMs_ = 0;
  bool held_ = false;
};
