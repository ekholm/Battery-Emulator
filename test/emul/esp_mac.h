#ifndef _EMUL_ESP_MAC_H_
#define _EMUL_ESP_MAC_H_

#include <stdint.h>
#include <string.h>

/* Host stand-in for the IDF's eFuse MAC read, so hostname.cpp can be built and tested here
 * rather than only on a board. The MAC is settable, which is the point: the default hostname
 * is derived from its last two bytes, and a test that cannot choose them cannot check the
 * formatting. */
typedef enum {
  ESP_MAC_WIFI_STA,
  ESP_MAC_WIFI_SOFTAP,
  ESP_MAC_BT,
  ESP_MAC_ETH,
} esp_mac_type_t;

// Recognisable rather than zero, so a test that forgets to set it still sees something.
inline uint8_t emul_mac_bytes[6] = {0x24, 0x6f, 0x28, 0x11, 0xab, 0xcd};

inline void emul_set_mac(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5) {
  emul_mac_bytes[0] = b0;
  emul_mac_bytes[1] = b1;
  emul_mac_bytes[2] = b2;
  emul_mac_bytes[3] = b3;
  emul_mac_bytes[4] = b4;
  emul_mac_bytes[5] = b5;
}

/* Counted, so a test can assert that the eFuse is read ONCE. Object identity cannot show that:
 * the cache is a function-local static that gets reassigned in place, so its address is the
 * same whether or not it is rebuilt on every call. */
inline int emul_mac_reads = 0;

inline void emul_reset_mac_reads() {
  emul_mac_reads = 0;
}

inline int esp_read_mac(uint8_t* mac, esp_mac_type_t /*type*/) {
  emul_mac_reads++;
  memcpy(mac, emul_mac_bytes, sizeof(emul_mac_bytes));
  return 0;
}

#endif  // _EMUL_ESP_MAC_H_
