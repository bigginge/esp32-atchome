#pragma once

#include <Arduino.h>

/**
 * Logging sink that writes to both serial routes.
 *
 * The build uses CDCOnBoot=cdc + USBMode=hwcdc, so `Serial` is the S3's *native*
 * USB CDC and UART0 is `Serial0`. The CrowPanel exposes both: a CH340 bridge on
 * one USB-C socket (which is also what arduino-cli uploads through) and the
 * native USB on the other. Logging to only one means the logs vanish depending
 * on which cable is plugged in -- which is exactly what happened while capturing
 * frame-time baselines. Writing to both costs nothing on an unopened port.
 */
class TeeLog : public Print {
 public:
  void begin(unsigned long baud);

  size_t write(uint8_t b) override;
  size_t write(const uint8_t *buf, size_t size) override;
};

extern TeeLog Log;
