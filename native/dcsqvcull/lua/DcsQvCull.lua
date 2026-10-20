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
  if (simStop or loadBegin or slotChange or gload) and DCS and type(DCS.setUserCallbacks) == 'function' then
    local cb = {}
    if simStop then cb.onSimulationStop = function() pcall(simStop) end end
    if loadBegin then cb.onMissionLoadBegin = function() pcall(loadBegin) end end
    if gload and Export and type(Export.LoGetAccelerationUnits) == 'function' then
      -- R58: is the camera the cockpit's? The camera more than 15 m from the ownship's origin is an external one
      -- (F2 / F6 / F10, a replay camera): the payload then draws no G field (a negative model time tells it).
      -- Unknown (an export forbidden or missing) counts as the cockpit. Re-checked every 0.2 s (wall clock: a
      -- paused mission still switches views).
      local cockpit, checkedAt = true, -1
      local function inCockpit(t)
        local now = (os and type(os.clock) == 'function') and os.clock() or t
        if checkedAt >= 0 and now >= checkedAt and now - checkedAt < 0.2 then return cockpit end
        checkedAt = now
        cockpit = true
        if type(Export.LoGetCameraPosition) ~= 'function' or type(Export.LoGetSelfData) ~= 'function' then
          return cockpit
        end
        local okC, c = pcall(Export.LoGetCameraPosition)
        local okS, s = pcall(Export.LoGetSelfData)
        if okC and okS and type(c) == 'table' and type(c.p) == 'table' and type(s) == 'table' and
            type(s.Position) == 'table' then
          local dx = (c.p.x or 0) - (s.Position.x or 0)
          local dy = (c.p.y or 0) - (s.Position.y or 0)
          local dz = (c.p.z or 0) - (s.Position.z or 0)
          local d2 = dx * dx + dy * dy + dz * dz
          if d2 == d2 then cockpit = d2 < 15 * 15 end
        end
        return cockpit
      end
      cb.onSimulationFrame = function()
        -- R90: nothing to do, and nothing allocated, while the payload does not use the G samples.
        if gloadWanted and gloadWanted() == false then return end
        local okA, a = pcall(Export.LoGetAccelerationUnits)
        if okA and type(a) == 'table' and a.y then
          local t = DCS.getModelTime and DCS.getModelTime() or 0
          local okV, inside = pcall(inCockpit, t)
          pcall(gload, a.x or 0, a.y, a.z or 0, (inside or not okV) and t or -1)
        end
      end
    end
    if slotChange then
      -- Only our own slot changes (multiplayer reports every player's).
      cb.onPlayerChangeSlot = function(id)
        local okId, me = pcall(function() return net.get_my_player_id() end)
        if not okId or me == nil or id == me then pcall(slotChange) end
      end
    end
    pcall(DCS.setUserCallbacks, cb)
  end
end
