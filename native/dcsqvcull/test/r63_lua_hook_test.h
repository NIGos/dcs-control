// R63 lua-cpath-separator: the Lua hook loads the loader by its exact path (package.loadlib), so a Saved Games /
// write-dir path containing ';' (legal in folder names; Lua splits package.cpath on it) still loads the module.
// Runs lua\DcsQvCull.lua in DCS's own lua.dll (Lua 5.1) with lfs / log stubbed and a stand-in loader
// (qv_fake_payload.dll, which exports luaopen_DcsQvCull) under a folder named "qv r63;lua".
#pragma once

namespace r63lua {

using State = void*;
using NewStateFn = State (*)();
using OpenLibsFn = void (*)(State);
using LoadStringFn = int (*)(State, const char*);
using LoadFileFn = int (*)(State, const char*);
using PcallFn = int (*)(State, int, int, int);
using ToLStringFn = const char* (*)(State, int, size_t*);
using SetTopFn = void (*)(State, int);
using CloseFn = void (*)(State);

std::wstring ExeDir() {
  wchar_t p[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, p, MAX_PATH);
  std::wstring s(p);
  return s.substr(0, s.find_last_of(L'\\') + 1);
}

std::string Narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s += static_cast<char>(c < 128 ? c : '?');
  return s;
}

void Run() {
  HMODULE lua = dcsinstall::Found() ? LoadLibraryW((dcsinstall::Bin() + L"lua.dll").c_str()) : nullptr;
  if (!lua) {
    dcsinstall::Skip("R63 Lua hook with ';' in the write dir (DCS's lua.dll)");
    return;
  }
  auto newState = reinterpret_cast<NewStateFn>(GetProcAddress(lua, "luaL_newstate"));
  auto openLibs = reinterpret_cast<OpenLibsFn>(GetProcAddress(lua, "luaL_openlibs"));
  auto loadString = reinterpret_cast<LoadStringFn>(GetProcAddress(lua, "luaL_loadstring"));
  auto loadFile = reinterpret_cast<LoadFileFn>(GetProcAddress(lua, "luaL_loadfile"));
  auto pcall = reinterpret_cast<PcallFn>(GetProcAddress(lua, "lua_pcall"));
  auto toLString = reinterpret_cast<ToLStringFn>(GetProcAddress(lua, "lua_tolstring"));
  auto setTop = reinterpret_cast<SetTopFn>(GetProcAddress(lua, "lua_settop"));
  auto closeState = reinterpret_cast<CloseFn>(GetProcAddress(lua, "lua_close"));
  const std::wstring exe = ExeDir();
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring root = std::wstring(tmp) + L"qv r63;lua" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  const std::wstring mod = root + L"Scripts\\DcsQvCull\\";
  CreateDirectoryW(root.c_str(), nullptr);
  CreateDirectoryW((root + L"Scripts").c_str(), nullptr);
  CreateDirectoryW(mod.c_str(), nullptr);
  const bool copied = CopyFileW((exe + L"qv_fake_payload.dll").c_str(), (mod + L"DcsQvCull.dll").c_str(), FALSE) != 0;
  if (!newState || !openLibs || !loadString || !loadFile || !pcall || !toLString || !setTop || !closeState || !copied) {
    printf("SKIP R63 Lua hook: lua.dll exports or the stand-in module unavailable\n");
  } else {
    State L = newState();
    openLibs(L);
    const std::string prelude = "lfs = {writedir = function() return [[" + Narrow(root) +
                                "]] end}\nLOG = {}\nlog = {INFO = 1, ERROR = 2, write = function(s, l, m) LOG[#LOG + 1] = "
                                "m end}\n";
    bool ok = loadString(L, prelude.c_str()) == 0 && pcall(L, 0, 0, 0) == 0;
    const std::string hook = Narrow(exe) + "..\\lua\\DcsQvCull.lua";
    ok = ok && loadFile(L, hook.c_str()) == 0 && pcall(L, 0, 0, 0) == 0;
    std::string first;
    if (ok && loadString(L, "return tostring(LOG[1]) .. ' | ' .. tostring(LOG[2])") == 0 && pcall(L, 0, 1, 0) == 0) {
      size_t n = 0;
      const char* s = toLString(L, -1, &n);
      if (s) first.assign(s, n);
      setTop(L, 0);
    }
    printf("     (Lua hook log: %s)\n", first.c_str());
    Check(ok && first.rfind("native module loaded", 0) == 0,
          "R63 Lua hook: a write dir with ';' (\"qv r63;lua\") still loads the module (exact path, no package.cpath)");
    closeState(L);
  }
  // The stand-in stays loaded by Lua's handle until the state closed; then the folder can go.
  DeleteFileW((mod + L"DcsQvCull.dll").c_str());
  RemoveDirectoryW(mod.c_str());
  RemoveDirectoryW((root + L"Scripts").c_str());
  RemoveDirectoryW(root.c_str());
  FreeLibrary(lua);
}

}  // namespace r63lua
