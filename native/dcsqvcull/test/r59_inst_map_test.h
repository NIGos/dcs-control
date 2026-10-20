// R59 inst-map-never-reclaimed: shadow_inst's instancing map (g_map) and
// texture-read map (g_texMap) reuse the slots of destroyed DX11Shaders
// (ForgetShader), so repeated mission loads do not fill them for good.
#pragma once

namespace r59map {
using namespace shadowinst;
const char* const kTechs[2] = {"lockon_shadows", "lockon_shadows_transparent"};

void Run() {
  const int savedState = g_state.load();
  MapEntry* const savedMap = g_map;
  TexEntry* const savedTex = g_texMap;
  const uint32_t savedUsed = g_mapUsed.load(), savedOcc = g_mapOccupied.load(), savedTexUsed = g_texMapUsed.load();
  uint32_t savedKind[kKinds];
  for (int k = 0; k < kKinds; ++k) savedKind[k] = g_mapUsedKind[k].load();
  ID3D11Device* const savedDev = g_device;
  g_state = 1;
  g_device = nullptr;  // no VS objects: the entries are what is checked
  g_map = new MapEntry[kMapSize];
  g_texMap = new TexEntry[kTexMapSize];
  g_mapUsed = 0;
  g_mapOccupied = 0;
  g_texMapUsed = 0;
  for (auto& u : g_mapUsedKind) u = 0;

  KeyResult r;
  r.ok = r.done = true;
  r.kind = kKindShadow;
  r.vs.resize(1);
  for (int t = 0; t < 2; ++t) {
    TechVs tv;
    tv.name = kTechs[t];
    tv.passVs = {0};
    r.techs.push_back(tv);
  }
  auto* s = new Snapshot;
  memset(s, 0, sizeof(*s));
  s->kind = kKindShadow;
  for (int t = 0; t < 2; ++t) {
    strcpy_s(s->techName[t], kTechs[t]);
    s->tech[t] = static_cast<uint64_t>(t + 1);
    s->passes[t] = 1;
  }
  // 40 "mission loads" of 200 shaders each, every load at new addresses
  // (16000 map and 16000 texture entries over the session, against caps of
  // 6144 and 3072), plus 300 shaders alive the whole session (their entries
  // sit inside clusters, between forgotten ones): every shader must be
  // published.
  constexpr int kLoads = 40, kPer = 200, kKeep = 300;
  constexpr size_t kStride = 0x100;
  std::vector<uint8_t> mem(static_cast<size_t>(kLoads) * kPer * kStride);
  std::vector<uint8_t> keep(static_cast<size_t>(kKeep) * kStride);
  int effectA = 0, effectB = 0, techArr = 0;
  bool allFound = true;
  int firstMiss = -1;
  auto publish = [&](uint8_t* sh) {
    *reinterpret_cast<void**>(sh + 0x50) = &effectA;
    *reinterpret_cast<void**>(sh + 0xb0) = &techArr;
    s->shader = sh;
    s->effect = &effectA;
    s->techBegin = &techArr;
    std::lock_guard<std::mutex> lock(g_mutex);
    PublishLocked(*s, r);
    PublishTexLocked(*s, r);
  };
  auto found = [](uint8_t* sh) { return FindEntry(sh, 1, 0) && FindEntry(sh, 2, 0) && FindTex(sh, 1) && FindTex(sh, 2); };
  for (int i = 0; i < kKeep; ++i) publish(keep.data() + i * kStride);
  for (int load = 0; load < kLoads; ++load) {
    std::vector<uint8_t*> shaders;
    for (int i = 0; i < kPer; ++i) {
      uint8_t* sh = mem.data() + (static_cast<size_t>(load) * kPer + i) * kStride;
      publish(sh);
      shaders.push_back(sh);
    }
    for (uint8_t* sh : shaders) {
      const bool f = found(sh);
      if (!f && firstMiss < 0) firstMiss = load;
      allFound &= f;
    }
    for (uint8_t* sh : shaders) ForgetShader(sh);  // the mission's shaders destroyed
  }
  for (int i = 0; i < kKeep; ++i) allFound &= found(keep.data() + i * kStride);
  for (int i = 0; i < kKeep; ++i) ForgetShader(keep.data() + i * kStride);
  // Everything forgotten: every slot empty again (no tombstone left).
  const bool counts = g_mapUsedKind[kKindShadow].load() == 0 && g_mapOccupied.load() == 0 && g_texMapUsed.load() == 0;
  if (firstMiss >= 0) printf("     first load with an unpublished shader: %d\n", firstMiss);
  Check(allFound && counts,
        "R59 inst map: shaders of 40 mission loads (32000 entries) and 300 long-lived ones are all published; "
        "destroyed shaders' slots reused or emptied");

  // A new shader at a reused address (new effect) is found after the old one was forgotten;
  // a snapshot of a shader destroyed meanwhile publishes nothing.
  uint8_t* sh = mem.data();
  const uint32_t used0 = g_mapUsed.load();
  *reinterpret_cast<void**>(sh + 0x50) = &effectB;
  s->shader = sh;
  s->effect = &effectB;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    PublishLocked(*s, r);
  }
  const bool reused = FindEntry(sh, 1, 0) && g_mapUsedKind[kKindShadow].load() == 2 && g_mapUsed.load() != used0;
  ForgetShader(sh);
  const uint32_t used1 = g_mapUsed.load();
  s->dead = true;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    PublishLocked(*s, r);
  }
  Check(reused && !FindEntry(sh, 1, 0) && g_mapUsedKind[kKindShadow].load() == 0 && used1 > used0,
        "R59 inst map: a reused address republishes; forgetting bumps the change counter; a dead snapshot publishes "
        "nothing");
  delete s;
  delete[] g_map;
  delete[] g_texMap;
  g_map = savedMap;
  g_texMap = savedTex;
  g_mapUsed = savedUsed;
  g_mapOccupied = savedOcc;
  g_texMapUsed = savedTexUsed;
  for (int k = 0; k < kKinds; ++k) g_mapUsedKind[k] = savedKind[k];
  g_device = savedDev;
  g_state = savedState;
}

}  // namespace r59map
