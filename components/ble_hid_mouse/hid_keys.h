#pragma once
// ASCII to USB HID keyboard usage (US layout). No ESP dependencies: tools/simulate_keys.py
// compiles this exact header natively and checks the table.
#include <cstdint>

namespace esphome {
namespace ble_hid_mouse {

static constexpr uint8_t HID_MOD_LCTRL = 0x01;
static constexpr uint8_t HID_MOD_LSHIFT = 0x02;
static constexpr uint8_t HID_MOD_LALT = 0x04;  // Option on a Mac
static constexpr uint8_t HID_MOD_LGUI = 0x08;  // Command on a Mac

static constexpr uint8_t HID_KEY_ENTER = 0x28;
static constexpr uint8_t HID_KEY_SPACE = 0x2C;

// Printable ASCII (0x20..0x7E) to a usage and modifier. Returns false for anything else.
inline bool ascii_to_hid(char c, uint8_t &mod, uint8_t &key) {
  mod = 0;
  if (c >= 'a' && c <= 'z') {
    key = (uint8_t) (0x04 + (c - 'a'));
  } else if (c >= 'A' && c <= 'Z') {
    mod = HID_MOD_LSHIFT;
    key = (uint8_t) (0x04 + (c - 'A'));
  } else if (c >= '1' && c <= '9') {
    key = (uint8_t) (0x1E + (c - '1'));
  } else if (c == '0') {
    key = 0x27;
  } else {
    // Unshifted and shifted symbol pairs, by usage code.
    struct Sym {
      char plain, shifted;
      uint8_t key;
    };
    static const Sym SYMS[] = {
        {' ', ' ', 0x2C}, {'-', '_', 0x2D}, {'=', '+', 0x2E}, {'[', '{', 0x2F}, {']', '}', 0x30},
        {'\\', '|', 0x31}, {';', ':', 0x33}, {'\'', '"', 0x34}, {'`', '~', 0x35}, {',', '<', 0x36},
        {'.', '>', 0x37}, {'/', '?', 0x38},
    };
    for (const Sym &s : SYMS) {
      if (c == s.plain) {
        key = s.key;
        return true;
      }
      if (c == s.shifted) {
        mod = HID_MOD_LSHIFT;
        key = s.key;
        return true;
      }
    }
    // Shifted digits.
    static const char SHIFT_DIGITS[] = ")!@#$%^&*(";  // index = digit 0..9
    for (int i = 0; i < 10; i++) {
      if (c == SHIFT_DIGITS[i]) {
        mod = HID_MOD_LSHIFT;
        key = i == 0 ? 0x27 : (uint8_t) (0x1E + i - 1);
        return true;
      }
    }
    return false;
  }
  return true;
}

}  // namespace ble_hid_mouse
}  // namespace esphome
