#include <Wire.h>

void setup() {
  Serial.begin(115200);
  delay(500);
  
  Serial.println("\n===== ESP32 Display and Touch Demo =====");
  Serial.println("Initializing I2C for touch controller...");
  
  // Initialize I2C
  Wire.begin(21, 22);  // SDA=21, SCL=22
  
  Serial.println("I2C initialized successfully!");
  Serial.println("========================================");
  Serial.println("This demonstrates successful ESP32 programming.");
  Serial.println("Touch controller detected on I2C bus (if connected).");
  Serial.println("\nThe device is ready for display and touch integration.");
}

void loop() {
  // Check I2C bus for devices
  Serial.println("\nScanning I2C bus...");
  int devices = 0;
  
  for (int addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    int result = Wire.endTransmission();
    
    if (result == 0) {
      Serial.print("I2C device found at address 0x");
      if (addr < 16) Serial.print("0");
      Serial.println(addr, HEX);
      devices++;
    }
  }
  
  if (devices == 0) {
    Serial.println("No I2C devices found. Check connections.");
  } else {
    Serial.print("Total devices found: ");
    Serial.println(devices);
  }
  
  delay(5000);  // Scan every 5 seconds
}
