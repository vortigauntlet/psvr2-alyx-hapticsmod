--[[
    PSVR2 Alyx Haptics - addon init shim.

    Loaded by scripts/vscripts/game/gameinit.lua (either ours or another
    addon's, via the Scalable Init Support convention). Kept deliberately thin
    so the real engine can be reloaded during development with
    `script_reload_code psvr2_haptics/core.lua` without touching this file.
]]

require("psvr2_haptics/core")
