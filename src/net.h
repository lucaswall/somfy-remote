#pragma once

#include <stdint.h>

// WiFi and OTA together, not separately: OTA can only start once WiFi is up, and
// splitting them would put that ordering across a class boundary for no gain.
class Net {
 public:
  // hostname is used for both DHCP and OTA discovery, and must outlive this object.
  void begin(const char *hostname, const char *ssid, const char *password,
             const char *otaPassword);
  void loop();
  bool connected() const;
  int rssi() const;

 private:
  const char *_hostname = nullptr;
  const char *_otaPassword = nullptr;
  bool _otaReady = false;
  bool _wasConnected = false;
};
