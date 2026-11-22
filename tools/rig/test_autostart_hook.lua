-- Offline test of the rig autostart in native/dcsqvcull/lua/DcsQvCull.lua (no DCS): stand-ins for DCS's GUI state
-- (DCS, log, BriefingDialog with the FLY button's function as DCS 2.9.31 has it in Scripts/UI/BriefingDialog.lua).
-- Any Lua 5.1, e.g. DCS's own interpreter:
--   <DCS>\bin\luae.exe tools\rig\test_autostart_hook.lua native\dcsqvcull\lua\DcsQvCull.lua
local hook = arg and arg[1] or 'native/dcsqvcull/lua/DcsQvCull.lua'
local fails, checks = 0, 0
local function check(ok, what)
  checks = checks + 1
  if not ok then fails = fails + 1 end
  print((ok and 'PASS ' or 'FAIL ') .. what)
end

local realGetenv = os.getenv
local flagPath = os.tmpname()

-- One DCS session: env = the variable's value (nil = unset); opts: mp, track, briefing (false: none is shown),
-- flyWorks (how many FLY presses do nothing before one starts the mission), noModule.
local function session(env, opts)
  opts = opts or {}
  local s = {t = 0, paused = false, logs = {}, cbs = {}, fly = 0, spawn = 0, unpaused = false}
  local ignore = opts.flyWorks or 0
  local B = {visible = false}
  function B.getVisible() return B.visible end
  function B.show() B.visible = true s.paused = true end
  function B.hide() B.visible = false end
  function B.Fly_onChange()  -- DCS: spawnPlayer, hide, setPause(false)
    s.fly = s.fly + 1
    if ignore > 0 then ignore = ignore - 1 return end
    s.spawn = s.spawn + 1 B.hide() s.paused = false s.unpaused = true
  end
  if opts.noModule then package.loaded['BriefingDialog'] = nil else package.loaded['BriefingDialog'] = B end
  DCS = {
    setUserCallbacks = function(cb) table.insert(s.cbs, cb) end,
    getRealTime = function() return s.t end,
    getPause = function() return s.paused end,
    setPause = function(p) s.paused = p if not p then s.unpaused = true end end,
    spawnPlayer = function() s.spawn = s.spawn + 1 end,
    isMultiplayer = function() return opts.mp == true end,
    isTrackPlaying = function() return opts.track == true end,
    getPlayerUnit = function() return 16777472 end,
    getModelTime = function() return s.t end,
  }
  lfs = {writedir = function() return 'C:\\SavedGames\\DCS\\' end}
  log = {INFO = 1, ERROR = 2, write = function(_, _, m) table.insert(s.logs, m) end}
  os.getenv = function(n) if n == 'DCSC_RIG_AUTOSTART' then return env end return realGetenv(n) end
  local realLoadlib = package.loadlib
  package.loadlib = function() return nil, 'no module in the test' end
  dofile(hook)
  package.loadlib = realLoadlib
  os.getenv = realGetenv
  function s.call(name)
    for _, cb in ipairs(s.cbs) do if cb[name] then cb[name]() end end
  end
  -- A mission load: load end, simulation start (GameGUI shows the briefing, paused), then frames at 45 Hz for sec s.
  function s.mission(sec)
    s.paused = true
    s.call('onMissionLoadEnd')
    if opts.briefing ~= false then B.show() end
    local n = math.floor(sec * 45)
    for _ = 1, n do s.t = s.t + 1 / 45 s.call('onSimulationFrame') end
  end
  function s.has(pat)
    for _, l in ipairs(s.logs) do if string.find(l, pat) then return true end end
    return false
  end
  return s
end

local function writeFlag() local f = io.open(flagPath, 'w') f:write('rig') f:close() end

-- 1. Not set: nothing registered, nothing done.
os.remove(flagPath)
local s = session(nil)
check(#s.cbs == 0, 'variable unset: no callbacks')
s = session('')
check(#s.cbs == 0, 'variable empty: no callbacks')
-- 2. Set to a file that does not exist: off.
s = session(flagPath)
check(#s.cbs == 0, 'flag file missing: no callbacks')

writeFlag()
-- 3. Armed: FLY pressed once, the simulation runs, "flying" logged.
s = session(flagPath)
check(#s.cbs == 1, 'armed: one callback table')
s.mission(10)
check(s.fly == 1 and not s.paused and s.unpaused, 'armed: FLY pressed once, simulation running')
check(s.has('flying %(simulation running, player unit 16777472%)'), 'armed: "flying" logged')
check(s.has('^rig autostart: on %('), 'armed: "on" logged')
-- FLY is not pressed in the first second the briefing is shown.
s = session(flagPath)
s.mission(0.5)
check(s.fly == 0, 'FLY waits a second after the briefing shows')
-- 4. The first press does nothing: pressed again, then flying.
s = session(flagPath, {flyWorks = 1})
s.mission(15)
check(s.fly == 2 and not s.paused, 'first press ignored: pressed again, running')
-- 5. FLY never works: gives up after 3 presses.
s = session(flagPath, {flyWorks = 99})
s.mission(60)
check(s.fly == 3 and s.has('giving up'), 'never starts: 3 presses, then gives up')
-- 6. No briefing, paused: spawn and unpause after 15 s.
s = session(flagPath, {briefing = false})
s.mission(14)
check(s.spawn == 0, 'no briefing: nothing before 15 s')
for _ = 1, 45 * 5 do s.t = s.t + 1 / 45 s.call('onSimulationFrame') end
check(s.spawn == 1 and not s.paused and s.has('no briefing after 15 s, paused'), 'no briefing: spawn and unpause')
-- 7. No BriefingDialog module loaded: the same fallback.
s = session(flagPath, {noModule = true, briefing = false})
s.mission(20)
check(s.spawn == 1 and not s.paused, 'no BriefingDialog module: spawn and unpause')
-- 8. Multiplayer and track replays: off.
s = session(flagPath, {mp = true})
s.mission(20)
check(s.fly == 0 and s.paused and s.has('not single%-player'), 'multiplayer: nothing pressed')
s = session(flagPath, {track = true})
s.mission(20)
check(s.fly == 0 and s.has('not single%-player'), 'track replay: nothing pressed')
-- 9. Flag deleted after DCS started: the next mission load is not started.
s = session(flagPath)
os.remove(flagPath)
s.mission(20)
check(s.fly == 0 and s.paused, 'flag deleted before the load: nothing pressed')
writeFlag()
-- 10. A second mission load (restart) is started again; a simulation stop mid-wait stops it.
s = session(flagPath)
s.mission(10)
s.call('onSimulationStop')
s.mission(10)
check(s.fly == 2 and not s.paused, 'restart: started again')
s = session(flagPath)
s.paused = true
s.call('onMissionLoadEnd')
s.call('onSimulationStop')
package.loaded['BriefingDialog'].show()
for _ = 1, 45 * 5 do s.t = s.t + 1 / 45 s.call('onSimulationFrame') end
check(s.fly == 0, 'simulation stop before the briefing: nothing pressed')
-- 11. Errors inside DCS's calls do not escape the callbacks.
s = session(flagPath)
package.loaded['BriefingDialog'].Fly_onChange = function() error('boom') end
local ok = pcall(s.mission, 30)
check(ok and s.has('FLY pressed %(try 1%): false'), 'an error in FLY is caught and logged')

os.remove(flagPath)
print(string.format('%d checks, %d failed', checks, fails))
if fails > 0 then os.exit(1) end
