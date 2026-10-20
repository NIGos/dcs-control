// R63 texbind-fasttick-alias-on-refresh-reset: the stats line's refresh rate no longer resets the timer cache's
// refresh counter (tex_bind.h's dedupe tick), so the tick never repeats and a stale fast-path entry cannot match a
// bind made one stats interval later.
#pragma once

namespace r63tick {

uint64_t g_binds = 0;
uint64_t __fastcall FakeBind(void*, uint64_t) {
  ++g_binds;
  return 1;
}

void Run() {
  const uint64_t r0 = timercache::g_refreshes.load();
  const bool on0 = timercache::g_on.load(), dd0 = texbind::g_dedupe.load();
  const texbind::Fn orig0 = texbind::g_orig;
  const bool meas0 = texbind::g_measure.load();
  texbind::g_orig = &FakeBind;
  texbind::g_measure = false;
  timercache::g_on = true;
  texbind::g_dedupe = true;
  alignas(16) static uint8_t tex[16];
  const uint64_t size = 512ull | (512ull << 32);
  // A bind at tick 5; the next stats interval ends 2000 refreshes later.
  timercache::g_refreshes = 5;
  g_binds = 0;
  texbind::Hook(tex, size);
  texbind::Hook(tex, size);  // same tick: skipped
  const bool sameTick = g_binds == 1;
  timercache::g_refreshes = 2005;
  uint64_t prev = 5;
  const uint64_t rate = timercache::RefreshesSince(&prev);
  const bool kept = rate == 2000 && prev == 2005 && timercache::g_refreshes.load() == 2005;
  // Five refreshes later the same texture is bound again: a new tick (was: the counter reset to 0 by the stats
  // block reached 5 again and this bind was skipped as a same-tick repeat).
  texbind::Hook(tex, size);
  Check(sameTick && kept && g_binds == 2 && texbind::FastTick() == 2005,
        "R63 tex tick: the stats rate (2000 refreshes) leaves the counter monotonic; a bind one interval later is not "
        "taken for a same-tick repeat");
  timercache::g_refreshes = 3;
  uint64_t p2 = 10;
  Check(timercache::RefreshesSince(&p2) == 3 && p2 == 3, "R63 tex tick: a counter below prev (new payload) gives its value");
  timercache::g_refreshes = r0;
  timercache::g_on = on0;
  texbind::g_dedupe = dd0;
  texbind::g_orig = orig0;
  texbind::g_measure = meas0;
}

}  // namespace r63tick
