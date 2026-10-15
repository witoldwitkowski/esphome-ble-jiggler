// Prints the unlock planner's action stream, one per line:
//   "<index> <kind> <dx> <dy> <mod> <usage> <delay> <step>"   kind: M move, D key down, U key up, W wait, E end
// Arguments: key=value (ints) and password=<text>. cancel_after=N cancels before action N.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../components/ble_hid_mouse/unlock_plan.h"

using namespace esphome::ble_hid_mouse;

int main(int argc, char **argv) {
  UnlockParams p;
  int min_iv = 0, cancel_after = -1;
  for (int i = 1; i < argc; i++) {
    const char *eq = std::strchr(argv[i], '=');
    if (!eq)
      continue;
    std::string k(argv[i], eq - argv[i]);
    const char *v = eq + 1;
    if (k == "password") p.password = v;
    else if (k == "min_iv") min_iv = std::atoi(v);
    else if (k == "cancel_after") cancel_after = std::atoi(v);
    else if (k == "wake_dwell_ms") p.wake_dwell_ms = (uint16_t) std::atoi(v);
    else if (k == "key_gap_ms") p.key_gap_ms = (uint16_t) std::atoi(v);
    else if (k == "field_dwell_ms") p.field_dwell_ms = (uint16_t) std::atoi(v);
    else if (k == "press_return") p.press_return = std::atoi(v) != 0;
    else if (k == "dry_run") p.dry_run = std::atoi(v) != 0;
  }
  UnlockPlanner pl;
  pl.start(p, (uint32_t) min_iv);
  for (int n = 0; n < 100000; n++) {
    if (n == cancel_after)
      pl.cancel();
    UAction a = pl.next();
    const char kc = a.kind == UKind::MOVE ? 'M' : a.kind == UKind::KEYDOWN ? 'D' : a.kind == UKind::KEYUP ? 'U'
                    : a.kind == UKind::WAIT ? 'W' : 'E';
    std::printf("%d %c %d %d %u %u %u %u\n", n, kc, (int) a.dx, (int) a.dy, (unsigned) a.mod, (unsigned) a.usage,
                (unsigned) a.delay_ms, (unsigned) a.step);
    if (a.kind == UKind::DONE)
      break;
  }
  return 0;
}
