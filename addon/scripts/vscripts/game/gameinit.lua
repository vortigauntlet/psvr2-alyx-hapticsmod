--[[
    PSVR2 Alyx Haptics - engine entry point.

    Half-Life: Alyx automatically executes scripts/vscripts/game/gameinit.lua
    from the addon search path, for every map, including the base campaign.

    Only ONE addon's copy of this file wins the search-path collision, so this
    file must never assume it belongs to us alone. It uses the community
    "Scalable Init Support" convention: enumerate every enabled addon and run
    that addon's own init file. That way whichever copy the engine happens to
    load, every mod still initialises.

    Reference: https://gitlab.com/recursivenomad/scalable_init_support
]]

local function initAll()
    local list = nil
    pcall(function() list = Convars:GetStr("default_enabled_addons_list") end)

    local seen = {}
    if list ~= nil then
        for addon in string.gmatch(list, "[^,%s]+") do
            if not seen[addon] then
                seen[addon] = true
                -- Scalable Init Support convention.
                pcall(require, "mods/init/" .. addon)
                -- Legacy convention used by older addons.
                pcall(require, addon)
            end
        end
    end

    -- Fail-safe: if this addon is side-loaded (installed by hand rather than
    -- subscribed through the Workshop) it will not appear in the enabled list,
    -- so load ourselves unconditionally. require() is idempotent, so an addon
    -- already loaded above is not run twice.
    if not seen["psvr2_haptics"] then
        pcall(require, "mods/init/psvr2_haptics")
    end
end

initAll()
