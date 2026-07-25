#include "gt911.hpp"

#include <Arduino.h>
#include <Wire.h>

namespace {
// GT911 16-bit register addresses.
constexpr uint16_t kRegStatus = 0x814E;  // buffer status + touch count
constexpr uint16_t kRegPoint1 = 0x814F;  // track id, then X lo/hi, Y lo/hi, size
constexpr uint32_t kHoldMs = 80;         // keep last point across not-ready reads
}  // namespace

bool GT911::read(uint16_t *x, uint16_t *y) {
  // Point the controller at the status register and read one byte.
  Wire.beginTransmission(addr_);
  Wire.write(static_cast<uint8_t>(kRegStatus >> 8));
  Wire.write(static_cast<uint8_t>(kRegStatus & 0xFF));
  if (Wire.endTransmission(false) != 0) {
    return held_ && (millis() - lastTouchMs_ < kHoldMs)
               ? (*x = lastX_, *y = lastY_, true)
               : (held_ = false, false);
  }

  uint8_t status = 0;
  if (Wire.requestFrom(static_cast<int>(addr_), 1) == 1) {
    status = Wire.read();
  }

  if (status & 0x80) {  // new data ready
    const uint8_t points = status & 0x0F;
    bool touched = false;
    if (points > 0) {
      Wire.beginTransmission(addr_);
      Wire.write(static_cast<uint8_t>(kRegPoint1 >> 8));
      Wire.write(static_cast<uint8_t>(kRegPoint1 & 0xFF));
      if (Wire.endTransmission(false) == 0 &&
          Wire.requestFrom(static_cast<int>(addr_), 5) == 5) {
        Wire.read();  // track id
        const uint8_t xl = Wire.read();
        const uint8_t xh = Wire.read();
        const uint8_t yl = Wire.read();
        const uint8_t yh = Wire.read();
        lastX_ = static_cast<uint16_t>((xh << 8) | xl);
        lastY_ = static_cast<uint16_t>((yh << 8) | yl);
        lastTouchMs_ = millis();
        held_ = true;
        touched = true;
      }
    } else {
      held_ = false;
    }

    // Clear the status flag so the controller reports the next frame.
    Wire.beginTransmission(addr_);
    Wire.write(static_cast<uint8_t>(kRegStatus >> 8));
    Wire.write(static_cast<uint8_t>(kRegStatus & 0xFF));
    Wire.write(static_cast<uint8_t>(0x00));
    Wire.endTransmission();

    if (touched) {
      *x = lastX_;
      *y = lastY_;
      return true;
    }
    return false;
  }

  // Not ready this poll: keep reporting the last point for a short window so a
  // held finger doesn't flicker to released between controller frames.
  if (held_ && (millis() - lastTouchMs_ < kHoldMs)) {
    *x = lastX_;
    *y = lastY_;
    return true;
  }
  held_ = false;
  return false;
}
