// Native harness for components/ble_hid_mouse/sleep_macro_plan.h. Built and run by simulate_macro.py.
//   macro_harness mode=0 [min_iv=0] [cancel_after=N] [key=value ...]   (keys: MacroParams fields)
// Prints one line per action:  t_ms kind buttons dx dy stage delay_ms
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "../components/ble_hid_mouse/sleep_macro_plan.h"

using namespace esphome::ble_hid_mouse;

int main(int argc, char **argv) {
  MacroParams p;
  int mode = 0, cancel_after = -1;
  uint32_t min_iv = 0;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto eq = a.find('=');
    if (eq == std::string::npos) continue;
    std::string k = a.substr(0, eq);
    long v = atol(a.c_str() + eq + 1);
    if (k == "mode") mode = (int) v;
    else if (k == "min_iv") min_iv = (uint32_t) v;
    else if (k == "cancel_after") cancel_after = (int) v;
    else if (k == "slam_count") p.slam_count = v;
    else if (k == "slam_cpr") p.slam_cpr = v;
    else if (k == "slam_interval_ms") p.slam_interval_ms = v;
    else if (k == "settle_ms") p.settle_ms = v;
    else if (k == "slam_passes") p.slam_passes = v;
    else if (k == "drift_cps") p.drift_cps = v;
    else if (k == "drift_interval_ms") p.drift_interval_ms = v;
    else if (k == "drift_scale_pct") p.drift_scale_pct = v;
    else if (k == "logo_x") p.logo_x = v;
    else if (k == "logo_y") p.logo_y = v;
    else if (k == "sleep_y") p.sleep_y = v;
    else if (k == "sleep_x") p.sleep_x = v;
    else if (k == "dwell_before_ms") p.dwell_before_ms = v;
    else if (k == "click_hold_ms") p.click_hold_ms = v;
    else if (k == "menu_dwell_ms") p.menu_dwell_ms = v;
    else if (k == "hover_dwell_ms") p.hover_dwell_ms = v;
    else if (k == "confirm_timeout_s") p.confirm_timeout_s = v;
    else if (k == "top_right") p.top_right = v != 0;
    else if (k == "dry_run") p.dry_run = v != 0;
    else { fprintf(stderr, "unknown key %s\n", k.c_str()); return 2; }
  }
  MacroPlanner pl;
  pl.start(p, (MacroMode) mode, min_iv);
  uint64_t t = 0;
  for (int n = 0; n < 100000; n++) {
    if (n == cancel_after) pl.cancel();
    MacroAction a = pl.next();
    const char *kind = a.kind == ActKind::REPORT ? "R" : a.kind == ActKind::WAIT ? "W" : a.kind == ActKind::CONFIRM ? "C" : "D";
    printf("%llu %s %u %d %d %u %u\n", (unsigned long long) t, kind, a.buttons, a.dx, a.dy, a.stage, a.delay_ms);
    if (a.kind == ActKind::DONE) return 0;
    t += a.delay_ms;
  }
  fprintf(stderr, "no DONE after 100000 actions\n");
  return 3;
}
