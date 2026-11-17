// Small Pimax Play service client for the probe's runner: the same PiPlayService.dll calls Pimax Play makes.
//   pimax_ctl get <int key>      prints the value
//   pimax_ctl set <int key> <v>  sets it and prints it back
//   pimax_ctl reboot             Pimax Play's "restart headset"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: pimax_ctl get <key> | set <key> <value> | reboot\n"); return 2; }
  SetDllDirectoryW(L"C:\\Program Files\\Pimax\\Runtime");
  HMODULE m = LoadLibraryExW(L"C:\\Program Files\\Pimax\\Runtime\\PiPlayService.dll", nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!m) { printf("cannot load PiPlayService.dll (%lu)\n", GetLastError()); return 1; }
  auto init = (int (*)())GetProcAddress(m, "PiServiceInit"); auto uninit = (int (*)())GetProcAddress(m, "PiServiceUnInit");
  auto get = (int (*)(const char*, int*))GetProcAddress(m, "PiService_GetIntConfig"); auto set = (int (*)(const char*, int))GetProcAddress(m, "PiService_SetIntConfig");
  auto reboot = (int (*)())GetProcAddress(m, "PiService_RebootHmd");
  if (!init || !uninit || !get || !set || !reboot) { printf("exports missing\n"); return 1; }
  int rc = init(); Sleep(500); int result = 0;
  if (!strcmp(argv[1], "get") && argc >= 3) { int v = -1; int r = get(argv[2], &v); printf("%d\n", v); result = r == 0 ? 0 : 1; }
  else if (!strcmp(argv[1], "set") && argc >= 4) { int r = set(argv[2], atoi(argv[3])); Sleep(500); int v = -1; get(argv[2], &v); printf("%d\n", v); result = (r == 0 && v == atoi(argv[3])) ? 0 : 1; }
  else if (!strcmp(argv[1], "reboot")) { int r = reboot(); printf("reboot rc %d (init %d)\n", r, rc); result = r == 0 ? 0 : 1; Sleep(500); }
  else result = 2;
  uninit();
  return result;
}
