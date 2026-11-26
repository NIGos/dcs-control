-- Loads the DcsQvCull native module (quad-view culling optimisation).
-- Install to: Saved Games\DCS\Scripts\Hooks\DcsQvCull.lua
local dll = lfs.writedir() .. [[Scripts\DcsQvCull\DcsQvCull.dll]]
local ok, err = pcall(function()
  -- By its exact path (R63): package.cpath is split on ';', which a Saved Games path may contain.
  local open, lerr = package.loadlib(dll, 'luaopen_DcsQvCull')
  if not open then error(lerr, 0) end
  open()
end)
if ok then
  log.write('DcsQvCull', log.INFO, 'native module loaded')
else
  log.write('DcsQvCull', log.ERROR, 'failed to load native module: ' .. tostring(err))
end

-- Exit and mission callbacks (loader.cpp). Each is optional: an older loader
-- lacks the export and package.loadlib returns nil.
local function native(name)
  local okL, f = pcall(package.loadlib, dll, name)
  if okL and type(f) == 'function' then return f end
  return nil
end
if ok then
  -- DCS exit: this userdata is created after the module was loaded, so Lua
  -- finalizes it first when DCS closes this state, before the loadlib
  -- finalizer that frees the module. The payload's threads stop touching DCS
  -- there, while DCS's objects still exist.
  local shutdown = native('DcsQvCull_Shutdown')
  if shutdown and type(newproxy) == 'function' then
    local guard = newproxy(true)
    getmetatable(guard).__gc = function() pcall(shutdown) end
    DcsQvCullExitGuard = guard  -- global: lives until the state closes
  end
  -- Mission end / next mission: recorder jobs in flight are dropped before
  -- DCS frees the mission's objects.
  local simStop = native('DcsQvCull_SimulationStop')
  local loadBegin = native('DcsQvCull_MissionLoadBegin')
  local slotChange = native('DcsQvCull_SlotChange')
  -- R51 VrFx GVision: the ownship's G (Export LoGetAccelerationUnits, body axes, y = normal load) every
  -- simulation frame; the payload ignores it unless [VrFx] GVision=1. Nil in MP when the server forbids it.
  local gload = native('DcsQvCull_GLoad')
  -- R90: false while the payload does not use the samples ([VrFx] GVision off); nil from an older loader.
  local gloadWanted = native('DcsQvCull_GLoadWanted')
  -- Cockpit space (docs/COCKPIT_SPACE.md 8.3): the player's unit type, the canopy and the camera check for the
  -- payload's 3D audio. Nothing is asked of DCS while it says it does not want them (3D audio or the space off).
  local audioWanted = native('DcsQvCull_AudioWanted')
  local audioAircraft = native('DcsQvCull_AudioAircraft')
  local audioState = native('DcsQvCull_AudioState')
  if not (audioWanted and audioAircraft and audioState) then audioWanted = nil end
  local function clock(t)
    return (os and type(os.clock) == 'function') and os.clock() or t or 0
  end
  -- R58: is the camera the cockpit's? The camera more than 15 m from the ownship's origin is an external one
  -- (F2 / F6 / F10, a replay camera). Unknown (an export forbidden or missing) counts as the cockpit (known false).
  -- Re-checked every 0.2 s (wall clock: a paused mission still switches views).
  local cockpit, cockpitKnown, checkedAt = true, false, -1
  local function inCockpit(t)
    local now = clock(t)
    if checkedAt >= 0 and now >= checkedAt and now - checkedAt < 0.2 then return cockpit, cockpitKnown end
    checkedAt = now
    cockpit, cockpitKnown = true, false
    if not Export or type(Export.LoGetCameraPosition) ~= 'function' or type(Export.LoGetSelfData) ~= 'function' then
      return cockpit, cockpitKnown
    end
    local okC, c = pcall(Export.LoGetCameraPosition)
    local okS, s = pcall(Export.LoGetSelfData)
    if okC and okS and type(c) == 'table' and type(c.p) == 'table' and type(s) == 'table' and
        type(s.Position) == 'table' then
      local dx = (c.p.x or 0) - (s.Position.x or 0)
      local dy = (c.p.y or 0) - (s.Position.y or 0)
      local dz = (c.p.z or 0) - (s.Position.z or 0)
      local d2 = dx * dx + dy * dy + dz * dz
      if d2 == d2 then cockpit, cockpitKnown = d2 < 15 * 15, true end
    end
    return cockpit, cockpitKnown
  end
  local audioTick = nil
  if audioWanted then
    local function unitType()
      if DCS and type(DCS.getPlayerUnitType) == 'function' then
        local okT, t = pcall(DCS.getPlayerUnitType)
        if okT and type(t) == 'string' and t ~= '' then return t end
      end
      if Export and type(Export.LoGetSelfData) == 'function' then
        local okS, s = pcall(Export.LoGetSelfData)
        if okS and type(s) == 'table' and type(s.Name) == 'string' then return s.Name end
      end
      return ''
    end
    -- 0 closed .. 1 open: LoGetMechInfo().canopy.value, else draw argument 38, else unknown (-1). Nil in MP when the
    -- server forbids ownship exports.
    local function canopy()
      if Export and type(Export.LoGetMechInfo) == 'function' then
        local okM, m = pcall(Export.LoGetMechInfo)
        if okM and type(m) == 'table' and type(m.canopy) == 'table' and type(m.canopy.value) == 'number' then
          return m.canopy.value, 1
        end
      end
      if Export and type(Export.LoGetAircraftDrawArgumentValue) == 'function' then
        local okA, a = pcall(Export.LoGetAircraftDrawArgumentValue, 38)
        if okA and type(a) == 'number' and a == a then return a, 2 end
      end
      return -1, 0
    end
    -- The type when it changed (checked every 2 s, sent again every 10 s: a reloaded payload hears it too), the
    -- canopy and the camera every 0.2 s.
    local lastType, typeAt, sentAt, stateAt = nil, -1, -1, -1
    local wasWanted = false
    audioTick = function(force)
      if audioWanted() ~= true then
        wasWanted = false
        return
      end
      -- The space just switched on: the type, canopy and camera at once (the module holds the space out until then).
      if not wasWanted then
        wasWanted = true
        force = true
      end
      local now = clock()
      if force or typeAt < 0 or now < typeAt or now - typeAt >= 2 then
        typeAt = now
        local t = unitType()
        if force or t ~= lastType or sentAt < 0 or now < sentAt or now - sentAt >= 10 then
          lastType, sentAt = t, now
          pcall(audioAircraft, t)
        end
      end
      if force or stateAt < 0 or now < stateAt or now - stateAt >= 0.2 then
        stateAt = now
        local c, src = canopy()
        local inside, known = inCockpit(now)
        pcall(audioState, c, src, known and (inside and 1 or 0) or -1)
      end
    end
  end
  if (simStop or loadBegin or slotChange or gload or audioTick) and DCS and type(DCS.setUserCallbacks) == 'function' then
    local cb = {}
    if simStop then cb.onSimulationStop = function() pcall(simStop) end end
    if loadBegin then cb.onMissionLoadBegin = function() pcall(loadBegin) end end
    if audioTick then cb.onSimulationStart = function() pcall(audioTick, true) end end
    local gframe = nil
    if gload and Export and type(Export.LoGetAccelerationUnits) == 'function' then
      gframe = function()
        -- R90: nothing to do, and nothing allocated, while the payload does not use the G samples.
        if gloadWanted and gloadWanted() == false then return end
        local okA, a = pcall(Export.LoGetAccelerationUnits)
        if okA and type(a) == 'table' and a.y then
          local t = DCS.getModelTime and DCS.getModelTime() or 0
          -- R58: an external camera draws no G field (a negative model time tells the payload).
          local okV, inside = pcall(inCockpit, t)
          pcall(gload, a.x or 0, a.y, a.z or 0, (inside or not okV) and t or -1)
        end
      end
    end
    if gframe and audioTick then
      cb.onSimulationFrame = function() gframe() audioTick(false) end
    elseif gframe then
      cb.onSimulationFrame = gframe
    elseif audioTick then
      cb.onSimulationFrame = function() audioTick(false) end
    end
    if slotChange or audioTick then
      -- Only our own slot changes (multiplayer reports every player's).
      cb.onPlayerChangeSlot = function(id)
        local okId, me = pcall(function() return net.get_my_player_id() end)
        if not okId or me == nil or id == me then
          if slotChange then pcall(slotChange) end
          if audioTick then pcall(audioTick, true) end
        end
      end
    end
    pcall(DCS.setUserCallbacks, cb)
  end
end

-- Test rig only: start a single-player mission past its briefing without a click, in DCS's own GUI state (the FLY
-- button's function), so no window focus or mouse is involved. Off unless DCS was started with the environment
-- variable DCSC_RIG_AUTOSTART naming a file that exists (tools/rig/run-scenario.ps1 sets it on the DCS it starts);
-- deleting that file disarms it. Each mission load presses FLY once the briefing is shown, checks that the
-- simulation runs, and tries again (at most 3 times); a mission with no briefing that stays paused is started with the
-- same calls. Every step is logged as "DcsQvCull rig autostart:" in dcs.log.
local function rigAutostart()
  local okE, flag = pcall(function() return os.getenv('DCSC_RIG_AUTOSTART') end)
  if not okE or type(flag) ~= 'string' or flag == '' then return end
  local function armed()
    local f = io.open(flag, 'r')
    if f then f:close() return true end
    return false
  end
  if not armed() then return end
  local function say(msg) log.write('DcsQvCull', log.INFO, 'rig autostart: ' .. msg) end
  local function now()
    local okT, t = pcall(DCS.getRealTime)
    if okT and type(t) == 'number' then return t end
    return os.clock()
  end
  local function paused() local okP, p = pcall(DCS.getPause) return okP and p == true end
  local function modelTime() local okM, m = pcall(DCS.getModelTime) if okM and type(m) == 'number' then return m end return nil end
  local function briefing()
    local b = package.loaded['BriefingDialog']
    if type(b) ~= 'table' then  -- (2.9.31 live, 2026-11-22: not found there; GameGUI holds it from require)
      local okR, r = pcall(require, 'BriefingDialog')
      b = okR and r or nil
    end
    if type(b) ~= 'table' then return nil end
    return b
  end
  local function visible(b)
    local okV, v = pcall(b.getVisible)
    return okV and v == true
  end
  local st = {phase = 0, t = 0, tries = 0, waitSince = 0}
  local cb = {}
  function cb.onMissionLoadEnd()
    if not armed() then st.phase = 0 return end
    local mp, track = false, false
    pcall(function() mp = DCS.isMultiplayer() == true end)
    pcall(function() track = DCS.isTrackPlaying() == true end)
    if mp or track then say('not single-player (multiplayer ' .. tostring(mp) .. ', track ' .. tostring(track) .. '): off') st.phase = 0 return end
    st.phase, st.tries, st.waitSince, st.t = 1, 0, now(), now()
    say('armed, waiting for the briefing')
  end
  function cb.onSimulationStop() st.phase = 0 end
  function cb.onSimulationFrame()
    if st.phase == 0 then return end
    local t = now()
    if st.phase == 1 then  -- wait for the briefing (GameGUI shows it at the simulation start), then let it draw a frame
      local b = briefing()
      if b and visible(b) then
        if t - st.t >= 1 then
          st.tries = st.tries + 1
          local okF, err = pcall(b.Fly_onChange)
          say(string.format('FLY pressed (try %d): %s %s', st.tries, tostring(okF), tostring(err)))
          st.phase, st.t, st.m0 = 2, t, modelTime()
        end
      else
        st.t = t
        if t - st.waitSince > 10 then
          if paused() then  -- no briefing came: the FLY button's calls by hand
            st.tries = st.tries + 1
            local okS, err = pcall(function()
              if b and type(b.hide) == 'function' then b.hide() end
              DCS.spawnPlayer()
              DCS.setPause(false)
            end)
            say(string.format('no briefing after 10 s, paused: spawn and unpause (try %d): %s %s', st.tries,
                              tostring(okS), tostring(err)))
            st.phase, st.t, st.m0 = 2, t, modelTime()
          else
            say('no briefing after 10 s and not paused: nothing to do')
            st.phase = 0
          end
        end
      end
    elseif st.phase == 2 and t - st.t >= 3 then  -- the check: model time runs, briefing gone
      local b = briefing()
      local m = modelTime()
      local runs = st.m0 and m and m - st.m0 > 1
      if not paused() and runs and not (b and visible(b)) then
        local unit = nil
        pcall(function() unit = DCS.getPlayerUnit() end)
        say(string.format('flying (model time %.1f s, +%.1f s in %.1f s real; player unit %s)', m, m - st.m0, t - st.t,
                          tostring(unit)))
        st.phase = 0
      elseif st.tries >= 3 then
        say('still paused after 3 tries: giving up')
        st.phase = 0
      else
        st.phase, st.t, st.waitSince = 1, t, t
      end
    end
  end
  pcall(DCS.setUserCallbacks, cb)
  say('on (' .. flag .. ')')
end
if DCS and type(DCS.setUserCallbacks) == 'function' and log and type(log.write) == 'function' then
  pcall(rigAutostart)
end
