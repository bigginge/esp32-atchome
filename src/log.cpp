#include "log.hpp"

TeeLog Log;

void TeeLog::begin(unsigned long baud) {
  Serial.begin(baud);
#if ARDUINO_USB_CDC_ON_BOOT
  // Only a distinct object when Serial is the native USB CDC; otherwise Serial
  // already *is* UART0 and beginning it twice would be pointless.
  Serial0.begin(baud);
#endif
}

size_t TeeLog::write(uint8_t b) {
  const size_t n = Serial.write(b);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial0.write(b);
#endif
  return n;
}

size_t TeeLog::write(const uint8_t *buf, size_t size) {
  const size_t n = Serial.write(buf, size);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial0.write(buf, size);
#endif
  return n;
}
