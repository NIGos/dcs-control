-- Loads the DcsQvCull native module (quad-view culling optimisation).
-- Install to: Saved Games\DCS\Scripts\Hooks\DcsQvCull.lua
local dll = lfs.writedir() .. [[Scripts\DcsQvCull\DcsQvCull.dll]]
local ok, err = pcall(function()
  package.cpath = package.cpath .. ';' .. lfs.writedir() .. [[Scripts\DcsQvCull\?.dll]]
  require('DcsQvCull')
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
  if (simStop or loadBegin or slotChange) and DCS and type(DCS.setUserCallbacks) == 'function' then
    local cb = {}
    if simStop then cb.onSimulationStop = function() pcall(simStop) end end
    if loadBegin then cb.onMissionLoadBegin = function() pcall(loadBegin) end end
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
