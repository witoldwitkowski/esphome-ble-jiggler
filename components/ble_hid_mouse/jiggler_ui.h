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
  // Sleep macro. Value-initialised to off, so builds that never set these are unaffected.
  uint8_t macro_stage;   // 0 = not running, else 1..7
  uint8_t macro_result;  // 0 none, 1 success, 2 failure
  bool dry_run;          // dry run is switched on
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

// Dotted outline (every other pixel) marking dry-run mode.
inline void dotted_rect(Display &it, int x, int y, int w, int h) {
  for (int i = 0; i < w; i += 2) {
    it.draw_pixel_at(x + i, y, ON);
    it.draw_pixel_at(x + i, y + h - 1, ON);
  }
  for (int j = 0; j < h; j += 2) {
    it.draw_pixel_at(x, y + j, ON);
    it.draw_pixel_at(x + w - 1, y + j, ON);
  }
}

// Crescent moon centred at (cx, cy).
inline void moon(Display &it, int cx, int cy, int r) {
  it.filled_circle(cx, cy, r, ON);
  it.filled_circle(cx + (2 * r) / 3, cy - r / 5, r - 2, OFF);
}

// Sleep macro screen: moon, 7 stage pips (done = filled, current = boxed, to-do = outline),
// and after the run a check mark or an X beside the moon.
inline void macro_screen(Display &it, uint8_t stage, uint8_t result, bool dry, uint32_t now) {
  moon(it, 26, 14, 11);
  if (dry) dotted_rect(it, 9, 1, 36, 28);
  if (stage > 0) {
    for (int i = 0; i < 7; i++) {
      const int x = 6 + i * 9;
      if (i < stage - 1) {
        it.filled_rectangle(x, 33, 6, 4, ON);
      } else if (i == stage - 1) {
        it.rectangle(x - 1, 32, 8, 6, ON);
        if ((now / 300) % 2 == 0) it.filled_rectangle(x + 1, 34, 4, 2, ON);
      } else {
        it.rectangle(x, 33, 6, 4, ON);
      }
    }
    // Zzz drifting up beside the moon.
    const int z = (now / 400) % 3;
    it.horizontal_line(50, 22 - z * 4, 6, ON);
    it.line(55, 22 - z * 4, 50, 26 - z * 4, ON);
    it.horizontal_line(50, 26 - z * 4, 6, ON);
  } else if (result == 1) {  // check mark
    for (int o = 0; o < 2; o++) {
      it.line(48, 16 + o, 53, 22 + o, ON);
      it.line(53, 22 + o, 64, 6 + o, ON);
    }
  } else if (result == 2) {  // X
    for (int o = 0; o < 2; o++) {
      it.line(49 + o, 7, 63 + o, 23, ON);
      it.line(63 + o, 7, 49 + o, 23, ON);
    }
  }
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
  if (s.macro_stage > 0 || s.macro_result > 0) {
    macro_screen(it, s.macro_stage, s.macro_result, s.dry_run, s.now_ms);
    return;
  }
  bt_icon(it, 4, 4, s.bt, s.now_ms);
  mouse_icon(it, 29, 4, s.enabled, s.pulse, s.now_ms);
  wifi_icon(it, 53, 3, s.wifi_bars);
  ha_icon(it, 56, 19, s.api_up);
  if (s.progress >= 0.0f) progress_bar(it, s.progress);
  if (s.dry_run) dotted_rect(it, 27, 2, 18, 24);  // dry-run marker around the mouse icon
}

}  // namespace jiggler_ui
