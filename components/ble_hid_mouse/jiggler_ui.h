#pragma once
// Icon-only UI for the 72x40 OLED, drawn with display primitives (no fonts, no bitmaps).
// All coordinates are inside the visible 72x40 window.
#include "esphome/components/display/display.h"
#include <cmath>
#include <cstdint>

namespace jiggler_ui {

using esphome::display::Display;
using esphome::Color;

enum class BtState : uint8_t { CONNECTED, BONDED_IDLE, PAIRING, NO_BOND };

struct UiState {
  BtState bt;
  bool enabled;
  bool pulse;        // a jiggle fired within the last second
  uint8_t wifi_bars; // 0..4 (0 = down)
  bool api_up;
  float progress;    // 0..1, negative = hide the bar
  bool ota;
  float ota_pct;     // 0..100
  uint32_t now_ms;
  bool test_pattern;
};

static const Color ON(255, 255, 255);
static const Color OFF(0, 0, 0);

// Bluetooth rune in a 12x20 box with its top-left corner at (x, y).
inline void bt_rune(Display &it, int x, int y, Color c, bool bold) {
  const int sx = x + 5;
  for (int o = 0; o <= (bold ? 1 : 0); o++) {
    it.line(sx + o, y, sx + o, y + 19, c);                 // stem
    it.line(sx + o, y, x + 10 + o, y + 5, c);              // top right
    it.line(x + 10 + o, y + 5, x + 1 + o, y + 14, c);      // down-left diagonal
    it.line(sx + o, y + 19, x + 10 + o, y + 14, c);        // bottom right
    it.line(x + 10 + o, y + 14, x + 1 + o, y + 5, c);      // up-left diagonal
  }
}

inline void bt_icon(Display &it, int x, int y, BtState st, uint32_t now) {
  switch (st) {
    case BtState::CONNECTED:
      // Solid: rune knocked out of a filled rounded block.
      it.filled_rectangle(x - 1, y - 1, 16, 22, ON);
      bt_rune(it, x + 1, y, OFF, true);
      break;
    case BtState::BONDED_IDLE:
      // Outline rune with a slash, blinking slowly (1 s on / 1 s off).
      if ((now / 1000) % 2 == 0) {
        bt_rune(it, x + 1, y, ON, false);
        it.line(x - 1, y + 19, x + 14, y, ON);
      }
      break;
    case BtState::PAIRING: {
      bt_rune(it, x + 1, y, ON, true);
      // Radio waves expanding on both sides, animated at ~3 steps/s.
      int step = (now / 300) % 4;  // 0..3 waves visible
      for (int k = 0; k < step; k++) {
        int r = 3 + k * 3;
        it.line(x + 13 + r - 3, y + 10 - 2 - k, x + 13 + r - 3 + 1, y + 10, ON);
        it.line(x + 13 + r - 3 + 1, y + 10, x + 13 + r - 3, y + 10 + 2 + k, ON);
        it.line(x - 1 - r + 3, y + 10 - 2 - k, x - 1 - r + 3 - 1, y + 10, ON);
        it.line(x - 1 - r + 3 - 1, y + 10, x - 1 - r + 3, y + 10 + 2 + k, ON);
      }
      break;
    }
    case BtState::NO_BOND:
      bt_rune(it, x, y, ON, false);
      // "+" badge at the upper right.
      it.line(x + 13, y + 1, x + 13, y + 7, ON);
      it.line(x + 10, y + 4, x + 16, y + 4, ON);
      break;
  }
}

// Mouse, 14x20, top-left (x, y).
inline void mouse_icon(Display &it, int x, int y, bool enabled, bool pulse, uint32_t now) {
  if (pulse && (now / 150) % 2 == 0) {
    it.rectangle(x - 3, y - 3, 20, 26, ON);  // flashing halo
    it.line(x - 7, y + 10, x - 5, y + 10, ON);
    it.line(x + 19, y + 10, x + 21, y + 10, ON);
  }
  if (enabled) {
    it.filled_rectangle(x, y, 14, 20, ON);
    it.line(x + 7, y, x + 7, y + 8, OFF);       // button split
    it.line(x, y + 8, x + 13, y + 8, OFF);
    it.filled_circle(x + 7, y + 12, 1, OFF);    // little wheel dot
  } else {
    it.rectangle(x, y, 14, 20, ON);
    it.line(x + 7, y, x + 7, y + 8, ON);
    it.line(x, y + 8, x + 13, y + 8, ON);
    it.filled_rectangle(x + 3, y + 11, 2, 6, ON);  // pause bars
    it.filled_rectangle(x + 9, y + 11, 2, 6, ON);
  }
}

// Wi-Fi bars, 12 wide x 12 tall, bottom-left at (x, y+11).
inline void wifi_icon(Display &it, int x, int y, uint8_t bars) {
  for (int i = 0; i < 4; i++) {
    int h = 3 * (i + 1);
    int bx = x + i * 3;
    if (bars == 0) {
      it.horizontal_line(bx, y + 11, 2, ON);  // down: flat stubs + slash
    } else if (i < bars) {
      it.filled_rectangle(bx, y + 12 - h, 2, h, ON);
    } else {
      it.horizontal_line(bx, y + 11, 2, ON);
    }
  }
  if (bars == 0) it.line(x - 1, y + 11, x + 12, y, ON);
}

// Home Assistant link: small house, filled when the API client is connected.
inline void ha_icon(Display &it, int x, int y, bool up) {
  it.triangle(x + 4, y, x, y + 4, x + 8, y + 4, ON);
  if (up) {
    it.filled_triangle(x + 4, y, x, y + 4, x + 8, y + 4, ON);
    it.filled_rectangle(x + 1, y + 4, 7, 5, ON);
  } else {
    it.rectangle(x + 1, y + 4, 7, 5, ON);
  }
}

inline void progress_bar(Display &it, float p) {
  it.rectangle(1, 33, 70, 6, ON);
  int w = (int) std::lround(68.0f * (p < 0 ? 0 : (p > 1 ? 1 : p)));
  if (w > 0) it.filled_rectangle(2, 34, w, 4, ON);
}

inline void ota_screen(Display &it, float pct, uint32_t now) {
  int bob = (now / 250) % 2;  // arrow bobs up/down
  int cx = 36;
  it.filled_triangle(cx, 3 + bob, cx - 10, 15 + bob, cx + 10, 15 + bob, ON);
  it.filled_rectangle(cx - 4, 15 + bob, 8, 10, ON);
  it.rectangle(4, 31, 64, 6, ON);
  int w = (int) std::lround(62.0f * (pct < 0 ? 0 : (pct > 100 ? 100 : pct)) / 100.0f);
  if (w > 0) it.filled_rectangle(5, 32, w, 4, ON);
}

inline void test_pattern(Display &it) {
  it.rectangle(0, 0, 72, 40, ON);                // window border
  it.filled_rectangle(0, 0, 4, 4, ON);           // corner blocks (all four must be visible)
  it.filled_rectangle(68, 0, 4, 4, ON);
  it.filled_rectangle(0, 36, 4, 4, ON);
  it.filled_rectangle(68, 36, 4, 4, ON);
  it.line(0, 0, 71, 39, ON);
  it.line(71, 0, 0, 39, ON);
}

inline void draw(Display &it, const UiState &s) {
  it.fill(OFF);
  if (s.test_pattern) {
    test_pattern(it);
    return;
  }
  if (s.ota) {
    ota_screen(it, s.ota_pct, s.now_ms);
    return;
  }
  bt_icon(it, 4, 4, s.bt, s.now_ms);
  mouse_icon(it, 29, 4, s.enabled, s.pulse, s.now_ms);
  wifi_icon(it, 53, 3, s.wifi_bars);
  ha_icon(it, 56, 19, s.api_up);
  if (s.progress >= 0.0f) progress_bar(it, s.progress);
}

}  // namespace jiggler_ui
