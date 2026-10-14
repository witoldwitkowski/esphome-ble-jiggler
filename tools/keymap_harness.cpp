// Prints "<char code> <modifier> <usage>" for every printable ASCII char, using the real header.
#include <cstdio>
#include "../components/ble_hid_mouse/hid_keys.h"
using namespace esphome::ble_hid_mouse;
int main() {
  for (int c = 0; c < 128; c++) {
    uint8_t m = 0, k = 0;
    if (ascii_to_hid((char) c, m, k))
      std::printf("%d %u %u\n", c, (unsigned) m, (unsigned) k);
  }
  return 0;
}
