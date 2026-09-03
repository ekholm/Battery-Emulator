#include "hostname.h"

#include <esp_mac.h>  // esp_read_mac()

String custom_hostname;  // If not set, defaults to default_hostname()

String hostname_for_mac(const uint8_t mac[6]) {
  char mac_suffix[5];
  snprintf(mac_suffix, sizeof(mac_suffix), "%02x%02x", mac[4], mac[5]);
  return "battery-emulator-" + String(mac_suffix);
}

const String& default_hostname() {
  static String cached;
  if (cached.isEmpty()) {
    uint8_t mac_bytes[6];
    esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);  // reads eFuse directly, valid even before WiFi starts
    cached = hostname_for_mac(mac_bytes);
  }
  return cached;
}

const String& active_hostname() {
  if (!custom_hostname.isEmpty()) {
    return custom_hostname;
  }
  return default_hostname();
}
