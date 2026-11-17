// Reads or sets integer keys of the Pimax compositor (pi_server) through the PVR client interface. These are not the
// Pimax Play settings pimax_ctl reaches. Slot positions: public PVR SDK header 1.32 (getIntConfig 18, setIntConfig 19,
// getStringConfig 20, getFloatConfig 16, getInt64Config 60).
//   pvr_cfg get <key>...        int, int64, float and string reading of each key ("unset" markers: -99999 / empty)
//   pvr_cfg set <key> <int>     sets and reads back
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
  if (argc < 3) { printf("usage: pvr_cfg get <key>... | set <key> <int>\n"); return 2; }
  HMODULE m = LoadLibraryW(L"LibPVRClient64.dll"); if (!m) { printf("cannot load LibPVRClient64.dll\n"); return 1; }
  struct Iface { void* slot[80]; };
  auto gi = (Iface* (*)(uint32_t, uint32_t))GetProcAddress(m, "getPvrInterface"); Iface* pi = gi ? gi(1, 32) : nullptr;
  if (!pi) { printf("no PVR interface 1.32\n"); return 1; }
  if (((int (*)())pi->slot[0])() != 0) { printf("initialise failed\n"); return 1; }
  void* hmd = nullptr; if (((int (*)(void**))pi->slot[2])(&hmd) != 0 || !hmd) { printf("createHmd failed\n"); return 1; }
  auto getInt = (int (*)(void*, const char*, int))pi->slot[18]; auto setInt = (int (*)(void*, const char*, int))pi->slot[19];
  auto getStr = (int (*)(void*, const char*, char*, int))pi->slot[20]; auto getFloat = (float (*)(void*, const char*, float))pi->slot[16];
  auto getInt64 = (int64_t (*)(void*, const char*, int64_t))pi->slot[60];
  if (!strcmp(argv[1], "get")) {
    for (int i = 2; i < argc; ++i) { char s[256] = ""; getStr(hmd, argv[i], s, sizeof(s));
      printf("%s: int %d int64 %lld float %g string \"%s\"\n", argv[i], getInt(hmd, argv[i], -99999), (long long)getInt64(hmd, argv[i], -99999), getFloat(hmd, argv[i], -99999.f), s); }
  } else if (!strcmp(argv[1], "set") && argc >= 4) {
    printf("%s before: %d\n", argv[2], getInt(hmd, argv[2], -99999));
    int r = setInt(hmd, argv[2], atoi(argv[3])); Sleep(300);
    printf("set -> rc %d, after: %d\n", r, getInt(hmd, argv[2], -99999));
  }
  ((void (*)(void*))pi->slot[3])(hmd); ((void (*)())pi->slot[1])();
  return 0;
}
