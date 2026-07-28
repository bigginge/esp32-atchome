#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#ifndef DEG_TO_RAD
#define DEG_TO_RAD 0.017453292519943295
#endif
uint32_t millis();
uint32_t micros();

class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t b) = 0;
  virtual size_t write(const uint8_t *buf, size_t size) = 0;
  size_t print(const char *) { return 0; }
  size_t println(const char *) { return 0; }
  size_t println() { return 0; }
  size_t printf(const char *, ...) { return 0; }
};
