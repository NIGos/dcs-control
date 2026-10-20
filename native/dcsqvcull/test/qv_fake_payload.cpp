// A stand-in payload for loader_test.cpp: Start and Stop call back into the
// test executable (exports QvTestStarted / QvTestStopping), nothing else.
#include <windows.h>

#include "../src/loader_api.h"

namespace {
template <class F>
F Host(const char* name) {
  return reinterpret_cast<F>(GetProcAddress(GetModuleHandleW(nullptr), name));
}
}  // namespace

extern "C" __declspec(dllexport) int DcsQvPayload_Start(const DcsQvLoaderApi*) {
  if (auto f = Host<void (*)()>("QvTestStarted")) f();
  if (auto r = Host<int (*)()>("QvTestStartResult")) return r();  // R62: a Start that fails
  return 0;
}

// R63: stands in for the loader in r63_lua_hook_test.h (the Lua hook's package.loadlib entry).
extern "C" __declspec(dllexport) int luaopen_DcsQvCull(void*) { return 0; }

extern "C" __declspec(dllexport) void DcsQvPayload_Stop() {
  if (auto f = Host<void (*)()>("QvTestStopping")) f();
}

// R90 F7: stand-ins for the loader's GVision callbacks in r90_test.h (the Lua hook's onSimulationFrame).
// DcsQvCull_GLoadWanted pushes the boolean QV_TEST_GLOAD_WANTED says ("0" false, else true) through DCS's
// lua.dll; DcsQvCull_GLoad counts its calls (QvFakeGLoadCalls).
namespace {
long g_fakeGLoadCalls = 0;
}
extern "C" __declspec(dllexport) int DcsQvCull_GLoadWanted(void* ls) {
  using PushBooleanFn = void(__cdecl*)(void*, int);
  HMODULE lua = GetModuleHandleW(L"lua.dll");
  auto push = lua ? reinterpret_cast<PushBooleanFn>(GetProcAddress(lua, "lua_pushboolean")) : nullptr;
  if (!push || !ls) return 0;
  char v[8] = "1";
  GetEnvironmentVariableA("QV_TEST_GLOAD_WANTED", v, sizeof(v));
  push(ls, v[0] != '0');
  return 1;
}
extern "C" __declspec(dllexport) int DcsQvCull_GLoad(void*) {
  ++g_fakeGLoadCalls;
  return 0;
}
extern "C" __declspec(dllexport) long QvFakeGLoadCalls() { return g_fakeGLoadCalls; }
