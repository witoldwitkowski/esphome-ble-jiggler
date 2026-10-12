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
  // Standalone (no Home Assistant) firmware only. Value-initialised to off elsewhere.
  bool standalone;      // hides the Home Assistant house icon
  bool setup_ap;        // Wi-Fi setup network is up: show setup screen with `tag`
  bool show_info;       // show the network info screen with `ip`
  const char *tag;      // short id of this device, e.g. "a1b2c3" (setup screen)
  const char *ip;       // dotted IPv4 text (info screen)
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

// 3x5 glyphs for 0-9, a-f, '.', ':' and '-'. Each row is 3 bits, top row first.
inline const uint8_t *glyph3x5(char c) {
  static const uint8_t G[][5] = {
      {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},  // 0-4
      {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},  // 5-9
      {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6}, {7, 4, 4, 4, 7}, {6, 5, 5, 5, 6}, {7, 4, 6, 4, 7},  // a-e
      {7, 4, 6, 4, 4},                                                                         // f
      {0, 0, 0, 0, 2}, {0, 2, 0, 2, 0}, {0, 0, 7, 0, 0}, {0, 0, 0, 0, 0}};                   // . : - space
  if (c >= '0' && c <= '9') return G[c - '0'];
  if (c >= 'a' && c <= 'f') return G[10 + (c - 'a')];
  if (c >= 'A' && c <= 'F') return G[10 + (c - 'A')];
  if (c == '.') return G[16];
  if (c == ':') return G[17];
  if (c == '-') return G[18];
  return G[19];
}

// Draws text at integer scale; advance is 4 * scale. Returns the x after the last glyph.
inline int text3x5(Display &it, int x, int y, const char *t, int scale) {
  for (; *t; t++) {
    const uint8_t *g = glyph3x5(*t);
    for (int r = 0; r < 5; r++)
      for (int c = 0; c < 3; c++)
        if (g[r] & (4 >> c)) it.filled_rectangle(x + c * scale, y + r * scale, scale, scale, ON);
    x += 4 * scale;
  }
  return x;
}

// Wi-Fi setup screen: arcs radiating from a dot (the device's own network) plus its id.
inline void setup_screen(Display &it, const char *tag, uint32_t now) {
  const int cx = 36, cy = 17;
  const int lit = (now / 400) % 4;  // 0..3 arcs lit, wrapping: "broadcasting"
  it.filled_circle(cx, cy, 2, ON);
  for (int i = 0; i < 3; i++) {
    if (i >= lit) break;
    const int r = 6 + i * 5;
    for (int a = 0; a <= 40; a++) {  // upper arc, 45..135 degrees
      const float t = (45.0f + a * 2.25f) * 0.0174533f;
      it.draw_pixel_at(cx + (int) std::lround(r * std::cos(t)), cy - (int) std::lround(r * std::sin(t)), ON);
    }
  }
  if (tag && *tag) {
    int len = 0;
    while (tag[len]) len++;
    text3x5(it, (72 - len * 8 + 2) / 2, 28, tag, 2);
  }
}

// Network info screen: dotted IPv4 at 2x, wrapped at a dot so each row fits in 72 px (9 glyphs).
inline void info_screen(Display &it, const char *ip) {
  if (!ip || !*ip) return;
  char row[2][12] = {{0}, {0}};
  int len = 0;
  while (ip[len]) len++;
  int split = len;
  if (len > 9) {
    split = 0;
    for (int i = 0; i < len && i < 9; i++)
      if (ip[i] == '.') split = i + 1;
    if (split == 0) split = 9;
  }
  for (int i = 0; i < split && i < 11; i++) row[0][i] = ip[i];
  for (int i = split; i < len && i - split < 11; i++) row[1][i - split] = ip[i];
  text3x5(it, 2, 6, row[0], 2);
  text3x5(it, 2, 22, row[1], 2);
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
  if (s.setup_ap) {
    setup_screen(it, s.tag, s.now_ms);
    return;
  }
  if (s.show_info) {
    info_screen(it, s.ip);
    return;
  }
  bt_icon(it, 4, 4, s.bt, s.now_ms);
  mouse_icon(it, 29, 4, s.enabled, s.pulse, s.now_ms);
  wifi_icon(it, 53, 3, s.wifi_bars);
  if (!s.standalone) ha_icon(it, 56, 19, s.api_up);
  if (s.progress >= 0.0f) progress_bar(it, s.progress);
}

}  // namespace jiggler_ui
