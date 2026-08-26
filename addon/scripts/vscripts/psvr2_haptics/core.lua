--[[
    PSVR2 Alyx Haptics - game-side telemetry engine.

    Emits one tagged line per semantic event on the console, which the
    middleware tails out of console.log (-condebug). This script never talks to
    hardware; it only classifies gameplay and physics state.

    Event names and field keys were taken from the bHaptics Alyx integration,
    which is the reference implementation for Alyx game-event haptics:
    https://github.com/bhaptics/bhaptics-half-life-alyx

    Two field conventions matter and are easy to get wrong:
      * grabbity_glove_* carries "hand_is_primary" (0/1), NOT a hand index.
        Resolving it to left/right requires tracking which hand is primary.
      * The fire event is "player_shoot_weapon", not "weapon_fire".
]]

if not IsServer() then return end

local TAG = "[PSVR2H]"
local VERSION = "7.0"

--------------------------------------------------------------------------
-- Reload-safe lifecycle
--------------------------------------------------------------------------
-- Alyx re-runs init scripts on map change and on script_reload_code. Without
-- this, every reload stacks another set of listeners and each event fires N
-- times. Handles live on _G so they survive the reload that replaces this chunk.

if _G.PSVR2H_HANDLES ~= nil then
    for _, handle in ipairs(_G.PSVR2H_HANDLES) do
        pcall(StopListeningToGameEvent, handle)
    end
end
_G.PSVR2H_HANDLES = {}

local handles = _G.PSVR2H_HANDLES

local function emit(line)
    print(TAG .. " " .. line)
end

local function safe(fn, fallback)
    local ok, value = pcall(fn)
    if ok and value ~= nil then return value end
    return fallback
end

-- Commas and newlines are the wire format delimiters, so any string coming
-- from the game has them stripped. Defined up HERE because diagnostics need
-- it long before the physics section that used to own it: a forward
-- reference would be a nil call, silently swallowed by an enclosing pcall -
-- precisely how a whole subsystem fails without saying a word.
local function sanitise(s)
    return (string.gsub(tostring(s or ""), "[,\r\n]", ";"))
end

--------------------------------------------------------------------------
-- Reading game event fields
--------------------------------------------------------------------------
-- Alyx hands a listener a plain Lua TABLE whose keys are the event's fields.
-- Every read in this script previously used the e:GetInt("x") accessor style,
-- which belongs to a different engine binding: the call raised, the enclosing
-- pcall swallowed it, and the field silently returned its default.
--
-- That was not a small bug. It made every gravity-glove event resolve to the
-- primary hand regardless of which glove was used, reported every injury as
-- exactly 100 damage (health fell back to 0), and meant weapon_switch never
-- reported an item at all. The failures were invisible precisely because the
-- fallbacks were plausible.
--
-- Both patterns are tried so this survives whichever binding a future build
-- uses, and the one that answered is recorded so it can be reported rather
-- than guessed at a second time.
local function markStyle(style)
    if _G.PSVR2H_FIELD_STYLE == nil then
        _G.PSVR2H_FIELD_STYLE = style
        emit("FIELDSTYLE:" .. style)
    end
end

local function fieldNum(e, key, fallback)
    if e == nil then return fallback end
    local v = e[key]
    if v ~= nil then markStyle("table"); return tonumber(v) or fallback end
    local ok, got = pcall(function() return e:GetInt(key) end)
    if ok and got ~= nil then markStyle("accessor"); return got end
    return fallback
end

local function fieldStr(e, key, fallback)
    if e == nil then return fallback end
    local v = e[key]
    if v ~= nil then markStyle("table"); return tostring(v) end
    local ok, got = pcall(function() return e:GetString(key) end)
    if ok and got ~= nil then markStyle("accessor"); return tostring(got) end
    return fallback
end

-- One-shot field discovery, per event name.
--
-- Rather than assume a field name from a reference project again, this reports
-- what each event actually carries the first time it fires. Cheap (once per
-- event type per session) and it turns "which field holds the hand?" from a
-- guess into a fact.
local function dumpFields(name, e)
    if _G.PSVR2H_DUMPED == nil then _G.PSVR2H_DUMPED = {} end
    if _G.PSVR2H_DUMPED[name] then return end
    _G.PSVR2H_DUMPED[name] = true
    if type(e) ~= "table" then
        emit("FIELDS:" .. name .. ",<type=" .. type(e) .. ">")
        return
    end
    local keys = {}
    for k, v in pairs(e) do
        keys[#keys + 1] = tostring(k) .. "=" .. tostring(v)
    end
    if #keys == 0 then
        emit("FIELDS:" .. name .. ",<empty table>")
        return
    end
    table.sort(keys)
    emit("FIELDS:" .. name .. "," .. (string.gsub(table.concat(keys, " "), "[,\r\n]", ";")))
end

local function on(eventName, fn)
    local handle = safe(function()
        return ListenToGameEvent(eventName, function(e)
            -- What this event really carries, reported once per event type.
            pcall(dumpFields, eventName, e)
            -- One bad handler must never take the whole layer down.
            local ok, err = pcall(fn, e or {})
            if not ok then emit("ERROR:" .. eventName .. ":" .. tostring(err)) end
        end, nil)
    end, nil)
    if handle ~= nil then handles[#handles + 1] = handle end
end

--------------------------------------------------------------------------
-- Player / hand state
--------------------------------------------------------------------------

local state = {
    primaryIsLeft = false,
    weapon = "NONE",
    twoHand = false,
    -- Inferred ammunition state. Alyx does not expose a magazine count to
    -- VScript, so these are counted from load and fire events rather than read.
    -- They can drift (a scripted refill, a shell loaded before this script
    -- attached), so they resync whenever the game reports a bulk load, and they
    -- are only ever used to *colour* a shot, never to decide whether one fired.
    shells = 0,
    -- Whether the shell count means anything yet.
    --
    -- It starts at zero and is only ever incremented by observed load events,
    -- so before the first load "0 shells" does not mean empty - it means
    -- unknown. Reporting it as empty made every single shotgun shot in a
    -- recorded session claim it was the last one, which fired the go-slack
    -- "empty" trigger overlay after EVERY shot and flattened the shotgun's
    -- profile for 320 ms at a time, all session long.
    shellsKnown = false,
    roundsSinceReload = 0,
    autoloader = nil,
    -- Last observed player health, so a hit can be sized by the damage it did
    -- rather than by how much health happens to remain. Resets with the player.
    lastHealth = 100,
}

local function primarySide()
    return state.primaryIsLeft and "left" or "right"
end

local function sideOfPrimaryFlag(e)
    -- hand_is_primary == 1 means the event came from the dominant hand.
    local isPrimary = fieldNum(e, "hand_is_primary", -1)
    if isPrimary == 0 then return state.primaryIsLeft and "right" or "left" end
    if isPrimary == 1 then return primarySide() end
    -- The field is absent. Fall back to the primary hand, but say so ONCE -
    -- silently assuming "primary" here is what sent an entire session's worth
    -- of gravity-glove events to the wrong hand without a single complaint
    -- from the log.
    if not _G.PSVR2H_WARNED_HAND then
        _G.PSVR2H_WARNED_HAND = true
        emit("WARN:hand_is_primary absent - glove side falls back to primary")
    end
    return primarySide()
end

--------------------------------------------------------------------------
-- Weapon classification
--------------------------------------------------------------------------
-- VScript never hands us a clean weapon enum, so identity is assembled from
-- three independent signals, strongest last:
--   1. the "item" string on weapon_switch
--   2. the classname/model attached to the primary hand (polled)
--   3. weapon-specific manipulation events - a shotgun shell can only be
--      loaded into a shotgun, so these are the most reliable of the three
-- Anything unrecognised is reported verbatim as WEAPON_RAW so profiles can be
-- extended against real data instead of guesses.

-- Exact entity classnames, read out of Half-Life: Alyx's own server.dll rather
-- than guessed. Both the wielded weapon classes and the item_ pickup variants
-- are listed, because either can turn up as a hand attachment.
local WEAPON_CLASS = {
    hlvr_weapon_energygun        = "PISTOL",
    hlvr_weapon_pistol           = "PISTOL",
    hlvr_weapon_generic_pistol   = "PISTOL",
    hlvr_weapon_shotgun          = "SHOTGUN",
    hlvr_weapon_rapidfire        = "SMG",
    hlvr_weapon_crowbar          = "MELEE",
    hlvr_weapon_crowbar_physics  = "MELEE",
    hlvr_weapon_tripmine         = "TOOL",
    hlvr_weapon_radio            = "TOOL",
    hlvr_multitool               = "TOOL",
    hlvr_weapon_grabbity_glove   = "HANDS",
    hlvr_weapon_grabbity_slingshot = "HANDS",

    item_hlvr_weapon_energygun      = "PISTOL",
    item_hlvr_weapon_generic_pistol = "PISTOL",
    item_hlvr_weapon_shotgun        = "SHOTGUN",
    item_hlvr_weapon_rapidfire      = "SMG",
    item_hlvr_weapon_tripmine       = "TOOL",
    item_hlvr_weapon_radio          = "TOOL",
    item_hlvr_weapon_grabbity_glove = "HANDS",
}

local function classify(raw)
    local s = string.lower(tostring(raw or ""))
    if s == "" then return nil end

    -- Exact match first: unambiguous, and immune to the substring collisions
    -- below (hlvr_clip_shotgun_speedloader is not a shotgun, for instance).
    local exact = WEAPON_CLASS[s]
    if exact ~= nil then return exact end

    -- Substring fallback for model paths and anything a mod adds.
    local function has(x) return string.find(s, x, 1, true) ~= nil end
    if has("grabbity") then return "HANDS" end
    if has("shotgun") then return "SHOTGUN" end
    if has("rapidfire") or has("smg") then return "SMG" end
    if has("energygun") or has("pistol") then return "PISTOL" end
    if has("grenade") then return "GRENADE" end
    if has("crowbar") then return "MELEE" end
    if has("multitool") or has("hacktool") or has("tripmine") then return "TOOL" end
    if has("hand_use_controller") or has("empty") then return "HANDS" end
    return nil
end

-- Weapon identity, by source priority.
--
-- The polled hand attachment was authoritative for several revisions and it
-- has now been caught giving a WRONG answer twice, in two different ways:
--
--   * stuck on PISTOL for a whole session while shotgun shells were loaded and
--     two-hand shotgun grabs fired - it was walking the holster rig's children
--     and returning whichever weapon was listed first
--   * stuck on SMG for a whole session, this time from a DIRECT classification,
--     with shot cadence in the log proving the pistol was being fired
--
-- Both times weapon_switch reported the correct transitions and was rejected
-- by the poll. GetHandAttachment() evidently returns a stale entity in this
-- build, so it can no longer outrank the game telling us what it equipped.
--
--   switch  the game announcing an equip. AUTHORITATIVE.
--   2h      a two-hand grab; the weapon is demonstrably in both hands.
--   poll    only trusted to report EMPTY hands, or to fill a gap when nothing
--           else has spoken. Never to contradict a switch.
--   others  clip/shell/chamber and friends fire for HOLSTERED weapons - you
--           can reload a stowed pistol - so they only ever fill a gap.
local function setWeapon(w, source)
    if w == nil or w == state.weapon then return end

    local authoritative = (source == "switch" or source == "2h")
    if not authoritative then
        -- The poll may still report that the hands are empty; that is the one
        -- thing it observes directly and cannot get stale.
        local fillsGap = (state.weapon == "NONE" or state.weapon == "HANDS")
        local saysEmpty = (source == "poll" and w == "HANDS")
        if not (fillsGap or saysEmpty) then
            emit("WEAPON_REJECT:" .. w .. "," .. (source or "?")
                .. ",current=" .. tostring(state.weapon))
            return
        end
    end

    state.weapon = w
    emit("WEAPON:" .. w .. "," .. primarySide() .. "," .. (source or "?"))
end


local function handEntity(handId)
    return safe(function()
        return Entities:GetLocalPlayer():GetHMDAvatar():GetVRHand(handId)
    end, nil)
end

local function handAttachment(handId)
    local hand = handEntity(handId)
    if hand == nil then return nil end
    return safe(function() return hand:GetHandAttachment() end, nil)
end

-- Polled confirmation of the equipped weapon. Cheap, and it catches changes
-- that never raise weapon_switch (crafting, scripted hand-offs).
-- What is REALLY in the hand.
--
-- GetHandAttachment() does not return the weapon. On a real session it
-- returned "hlvr_weaponswitch_controller" - Alyx's holster/wheel rig - with an
-- empty model name, every single time. classify() had nothing to work with, so
-- the poll never once produced an answer and the weapon latched to whatever a
-- save-load last announced. Shotgun blasts were rendered with the pistol
-- profile for entire sessions.
--
-- The actual weapon hangs off that controller as a child, so the tree has to
-- be walked. Grabbity gloves classify as HANDS and are skipped, because a
-- glove is always present and would mask the gun.
local function weaponUnder(ent, depth)
    if ent == nil or depth > 2 then return nil end
    local children = safe(function() return ent:GetChildren() end, nil)
    if children == nil then return nil end
    for _, child in pairs(children) do
        local w = classify(safe(function() return child:GetClassname() end, ""))
            or classify(safe(function() return child:GetModelName() end, ""))
        if w ~= nil and w ~= "HANDS" then return w end
        local deeper = weaponUnder(child, depth + 1)
        if deeper ~= nil then return deeper end
    end
    return nil
end

local function pollWeapon()
    local att = handAttachment(state.primaryIsLeft and 0 or 1)
    if att == nil then
        setWeapon("HANDS", "poll")
        return
    end
    local cls = safe(function() return att:GetClassname() end, "")
    local mdl = safe(function() return att:GetModelName() end, "")

    -- The attachment ITSELF is the only authoritative answer.
    --
    -- Walking the holster rig's children finds whichever weapon happens to be
    -- listed first, not the one actually equipped. A recorded session proved
    -- it: the poll reported PISTOL on every single tick while shotgun shells
    -- were being loaded and two-hand shotgun grabs were firing, and because
    -- the poll outranks every other source it REJECTED all of them. Sixty
    -- shots, every one rendered with the pistol profile, which is exactly why
    -- the two guns were reported as feeling identical.
    --
    -- So a direct classification is authoritative and still wins outright; a
    -- child-walk result is downgraded to a hint that fills a gap but can be
    -- overridden by weapon_switch or a two-hand grab, both of which name the
    -- weapon the game itself believes is in use.
    local direct = classify(cls) or classify(mdl)
    if direct ~= nil then
        setWeapon(direct, "poll")
        return
    end

    local nested = weaponUnder(att, 0)
    if nested ~= nil then
        -- A guess: whichever weapon is listed first under the holster rig,
        -- which is not necessarily the equipped one. Good enough to fill a
        -- gap, never good enough to contradict the game.
        setWeapon(nested, "poll-guess")
        return
    end
    -- Report what the poll could not classify, once per distinct attachment.
    --
    -- A whole session produced no polled weapon at all and there was no way to
    -- tell why - the failure was completely silent, and it took a two-hand
    -- grab event to notice the weapon was stuck. Whatever this prints is the
    -- string that needs adding to WEAPON_CLASS.
    local key = cls .. "|" .. mdl
    if _G.PSVR2H_UNKNOWN_ATT ~= key then
        _G.PSVR2H_UNKNOWN_ATT = key
        -- Report the CHILDREN too. Knowing the attachment is a weaponswitch
        -- controller was only half the answer; if the walk below it still
        -- finds nothing, these names are what the weapon table is missing.
        local kids = {}
        local children = safe(function() return att:GetChildren() end, nil)
        if children ~= nil then
            for _, child in pairs(children) do
                kids[#kids + 1] = safe(function() return child:GetClassname() end, "?")
            end
        end
        emit("POLL_UNKNOWN:" .. sanitise(cls) .. "," .. sanitise(mdl)
            .. ",kids=" .. sanitise(table.concat(kids, " ")))
    end
end

--------------------------------------------------------------------------
-- Physics telemetry
--------------------------------------------------------------------------
-- The governing rule for this entire section:
--
--     IF THE PLAYER'S HAND WOULD NOT PHYSICALLY FEEL IT, DO NOT REPORT IT.
--
-- The previous revision tracked props for 0.8 s AFTER they left the hand and
-- emitted an impact whenever one decelerated, so a bottle smashing into a wall
-- three metres away buzzed the controller. The hand has no physical
-- relationship with that collision - and every impact haptic in the build came
-- from that one path. It is gone.
--
-- What replaced it is the case that genuinely deserves feedback and previously
-- had none: an object striking something WHILE STILL HELD. That is also the
-- closest thing Half-Life: Alyx has to melee, because the game has no swingable
-- melee weapon - "hitting something" means bashing it with a grabbed prop.

local PHYS_INTERVAL = 0.016
local EMIT_COOLDOWN = 0.06

-- Held-impact discrimination.
--
-- A held prop follows the hand, so it decelerates every time the hand does.
-- Comparing the object's speed loss against the HAND's speed loss over the same
-- tick is what separates "you stopped your arm" from "it hit something": in a
-- real collision the object sheds speed the hand did not.
--
-- These are heuristic thresholds in Source units/s, not measured constants.
-- Every impact therefore carries an explicit confidence value so the middleware
-- can treat a marginal reading as marginal instead of as fact.
local HELD_MIN_SPEED  = 110
local HELD_MIN_EXCESS = 95
local HELD_COOLDOWN   = 0.10

-- A gravity-glove catch lands the object in the hand a tick or two after the
-- catch event itself. Inside this window an arrival is that catch completing,
-- not a fresh manual pickup - which is what stops one catch reporting twice.
local CATCH_WINDOW = 0.35

-- Holding something heavy is a STATE, not an event.
--
-- The old build reported mass once at pickup and then forgot, so a 20 kg crate
-- felt like a 20 kg crate for a quarter of a second and like nothing at all for
-- as long as you actually carried it. Death Stranding's cargo triggers and
-- Synapse's telekinetic grip both land on the same answer: while you are
-- holding it, the grip stays loaded.
--
-- This is sent as a repeating heartbeat rather than a hold/release pair on
-- purpose. It fails safe - if this script dies, the map changes, or a release
-- is somehow missed, the heartbeat stops and the middleware's grip load simply
-- expires. Nothing can strand the trigger in a loaded state.
local HOLD_MIN_MASS = 2.0
local HOLD_INTERVAL = 0.25

local lastEmit = {}
local hands = {
    [0] = { obj = nil, mass = 1, material = "unknown", model = "",
            lastSpeed = 0, lastSpin = 0, lastHandSpeed = 0,
            held = false, lastImpact = 0, catchUntil = 0 },
    [1] = { obj = nil, mass = 1, material = "unknown", model = "",
            lastSpeed = 0, lastSpin = 0, lastHandSpeed = 0,
            held = false, lastImpact = 0, catchUntil = 0 },
}

local function mag(v)
    if v == nil then return 0 end
    return math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
end

-- Coarse tactile material class inferred from model path and classname. Alyx
-- does not expose surface properties to VScript, so this is a heuristic, not
-- ground truth: it shapes the timbre of an impact, never whether one fires.
local function materialFor(model, classname)
    local s = string.lower((model or "") .. " " .. (classname or ""))
    local function has(x) return string.find(s, x, 1, true) ~= nil end
    if has("glass") or has("bottle") or has("window") or has("jar") then return "glass" end
    if has("metal") or has("barrel") or has("pipe") or has("combine") then return "metal" end
    if has("wood") or has("crate") or has("plank") or has("pallet") or has("chair") then return "wood" end
    if has("concrete") or has("stone") or has("brick") or has("rubble") then return "stone" end
    if has("rubber") or has("tire") then return "rubber" end
    if has("headcrab") or has("flesh") or has("zombie") or has("antlion") then return "organic" end
    if has("cardboard") or has("paper") then return "cardboard" end
    if has("plastic") or has("bucket") or has("cone") then return "plastic" end
    return "unknown"
end

local function heldCandidate(handId)
    local att = handAttachment(handId)
    if att == nil then return nil end
    local children = safe(function() return att:GetChildren() end, nil)
    if children == nil then return nil end
    for _, child in pairs(children) do
        local cls = safe(function() return child:GetClassname() end, "")
        if cls == "prop_physics" or cls == "prop_physics_override"
            or cls == "prop_physics_multiplayer" or cls == "func_physbox" then
            return child
        end
    end
    return nil
end

local function throttled(key, now, cooldown)
    local prev = lastEmit[key]
    if prev ~= nil and (now - prev) < (cooldown or EMIT_COOLDOWN) then return true end
    lastEmit[key] = now
    return false
end

-- Called by the gravity-glove catch handler further down, so that the arrival
-- of the object resolves into the catch rather than into a separate pickup.
local function armCatch(side)
    local h = hands[side == "left" and 0 or 1]
    h.catchUntil = Time() + CATCH_WINDOW
end

local function inspectHand(handId)
    local h = hands[handId]
    local hand = handEntity(handId)
    if hand == nil then return end

    local handVel = safe(function() return hand:GetVelocity() end, Vector(0, 0, 0))
    local handSpeed = mag(handVel)
    local obj = heldCandidate(handId)
    local now = Time()
    local side = handId == 0 and "left" or "right"

    if obj ~= nil then
        local model = safe(function() return obj:GetModelName() end, "")
        local cls = safe(function() return obj:GetClassname() end, "")
        local speed = mag(safe(function() return GetPhysVelocity(obj) end, Vector(0, 0, 0)))
        local spin = mag(safe(function() return GetPhysAngularVelocity(obj) end, Vector(0, 0, 0)))

        if h.obj == nil then
            -- New object in the hand.
            h.obj = obj
            h.held = true
            h.mass = math.max(0.05, safe(function() return obj:GetMass() end, 1.0))
            h.material = materialFor(model, cls)
            h.model = model

            if now < h.catchUntil then
                -- The gravity glove delivering what it pulled. The middleware
                -- has already played the capture snap; this is what gives that
                -- snap its WEIGHT, which is the entire difference between
                -- catching a bottle and catching a metal crate.
                h.catchUntil = 0
                emit(string.format("GLOVE_CATCH_MASS:%.2f,%s,%s,%.1f,%s",
                    h.mass, side, h.material, spin, sanitise(model)))
            elseif not throttled("pickup" .. handId, now) then
                emit(string.format("PHYS_PICKUP:%.2f,%.2f,%s,%s,%.1f,%s",
                    handSpeed, h.mass, side, h.material, spin, sanitise(model)))
            end
        elseif h.held then
            -- Already held. This is where a genuine hand-felt impact lives:
            -- the object lost speed that the hand did not.
            local objDrop = h.lastSpeed - speed
            local handDrop = h.lastHandSpeed - handSpeed
            local excess = objDrop - handDrop

            if h.lastSpeed > HELD_MIN_SPEED and excess > HELD_MIN_EXCESS
                and (now - h.lastImpact) > HELD_COOLDOWN then
                h.lastImpact = now

                -- impulse ~ mass * (the velocity the collision actually
                -- removed). A defensible heuristic, NOT a contact impulse: the
                -- collision normal is unknown, and any energy that went into
                -- rotation is not accounted for.
                local impulse = math.max(0, h.mass * excess)

                -- Confidence rises when the reading is hard to explain as
                -- anything but a collision: the hand still moving, and most of
                -- the object's deceleration unexplained by the hand's own.
                local conf = 0.35
                if handSpeed > 45 then conf = conf + 0.30 end
                if objDrop > 0 and (excess / objDrop) > 0.6 then conf = conf + 0.35 end
                if conf > 1.0 then conf = 1.0 end

                emit(string.format("PHYS_IMPACT:%.1f,%.2f,%s,%s,%.1f,%s,1,%.2f",
                    impulse, h.mass, side, h.material, spin, sanitise(h.model), conf))
            end

            -- Grip load heartbeat. Hand speed and spin travel with it so the
            -- middleware can add the inertia of a heavy object being SWUNG
            -- without producing anything at all while you simply stand there
            -- holding it.
            if h.mass >= HOLD_MIN_MASS
                and not throttled("hold" .. handId, now, HOLD_INTERVAL) then
                emit(string.format("PHYS_HOLD:%.2f,%s,%s,%.1f,%.1f",
                    h.mass, side, h.material, handSpeed, spin))
            end
        end

        h.lastSpeed = speed
        h.lastSpin = spin
    elseif h.obj ~= nil and h.held then
        -- Detached from the hand. The controller's own velocity is the
        -- meaningful throw velocity; the prop has not accelerated yet.
        --
        -- Nothing is tracked past this point ON PURPOSE. Whatever the object
        -- goes on to hit, the hand is no longer connected to it.
        h.held = false
        if not throttled("throw" .. handId, now) then
            emit(string.format("PHYS_THROW:%.2f,%.2f,%s,%s,%.1f,%s",
                handSpeed, h.mass, side, h.material, h.lastSpin, sanitise(h.model)))
        end
        h.obj = nil
        h.lastSpeed = 0
    end

    h.lastHandSpeed = handSpeed
end

--------------------------------------------------------------------------
-- Doors
--------------------------------------------------------------------------
-- Opening doors is constant in Half-Life: Alyx and had no feedback at all.
-- It is also a textbook case for the golden rule: your hand is literally
-- closed on the handle, pushing a heavy object that pushes back.
--
-- Alyx raises no game event for it, so this is detected by polling. The class
-- names and the fact that doors are hand-interacted were both read out of the
-- shipped server.dll rather than guessed - it exposes prop_door_rotating,
-- prop_door_rotating_physics and func_door_rotating, alongside convars named
-- vr_door_handle_interact_start_distance, vr_door_handle_interact_hold_distance
-- and vr_door_mass, which only exist because hands move doors.
--
-- The discriminator is deliberately strict, because the obvious failure mode is
-- buzzing at a door that swings shut on its own near the player: the door must
-- be turning, a hand must be close to it, AND that hand must itself be moving.
-- A door moving on its own fails the third test.
--
-- Still a heuristic. A door's origin is its hinge rather than its handle, so
-- "close to it" is measured generously and this can only ever be approximate.
-- Set doors=false in the config to switch it off entirely.
local DOOR_CLASSES = {
    "prop_door_rotating",
    "prop_door_rotating_physics",
    "func_door_rotating",
}
local DOOR_SCAN_INTERVAL = 0.25 -- rescan for nearby doors at 4 Hz
local DOOR_SCAN_RADIUS   = 180  -- source units; only doors you could reach
local DOOR_HAND_RADIUS   = 90   -- hinge distance, so generous on purpose
local DOOR_MIN_SPIN      = 12   -- deg/s before it counts as actually turning
local DOOR_MIN_HAND      = 12   -- units/s; your hand must be doing the moving
local DOOR_EMIT_INTERVAL = 0.20

local nearbyDoors = {}
local lastDoorScan = -1
local doorMoving = {}

local function inspectDoors()
    local now = Time()
    local player = safe(function() return Entities:GetLocalPlayer() end, nil)
    if player == nil then return end

    if (now - lastDoorScan) >= DOOR_SCAN_INTERVAL then
        lastDoorScan = now
        nearbyDoors = {}
        local origin = safe(function() return player:GetOrigin() end, nil)
        if origin ~= nil then
            for _, cls in ipairs(DOOR_CLASSES) do
                local list = safe(function()
                    return Entities:FindAllByClassnameWithin(cls, origin, DOOR_SCAN_RADIUS)
                end, nil)
                if list ~= nil then
                    for _, d in pairs(list) do nearbyDoors[#nearbyDoors + 1] = d end
                end
            end
        end
    end

    for _, door in pairs(nearbyDoors) do
        if safe(function() return IsValidEntity(door) end, false) then
            local spin = mag(safe(function() return door:GetAngularVelocity() end, Vector(0, 0, 0)))
            local key = tostring(door)
            if spin < DOOR_MIN_SPIN then
                doorMoving[key] = nil
            else
                local centre = safe(function() return door:GetCenter() end, nil)
                if centre ~= nil then
                    -- Whichever hand is both near the door and actually moving.
                    local best, bestSide = nil, nil
                    for handId = 0, 1 do
                        local hand = handEntity(handId)
                        if hand ~= nil then
                            local hp = safe(function() return hand:GetOrigin() end, nil)
                            local hv = mag(safe(function() return hand:GetVelocity() end, Vector(0, 0, 0)))
                            if hp ~= nil and hv >= DOOR_MIN_HAND then
                                local dx, dy, dz = hp.x - centre.x, hp.y - centre.y, hp.z - centre.z
                                local dist = math.sqrt(dx * dx + dy * dy + dz * dz)
                                if dist <= DOOR_HAND_RADIUS and (best == nil or dist < best) then
                                    best = dist
                                    bestSide = handId == 0 and "left" or "right"
                                end
                            end
                        end
                    end
                    if bestSide ~= nil then
                        doorMoving[key] = true
                        if not throttled("door" .. key, now, DOOR_EMIT_INTERVAL) then
                            local mass = math.max(1.0, safe(function() return door:GetMass() end, 40.0))
                            emit(string.format("DOOR_MOVE:%s,%.1f,%.1f", bestSide, spin, mass))
                        end
                    end
                end
            end
        end
    end
end

-- Transport heartbeat.
--
-- Think() runs on a fixed interval, so these go out at a known, even rate
-- carrying the game's own clock. The middleware compares the spacing it
-- OBSERVES against the spacing the game INTENDED: if they match, the transport
-- is delivering promptly; if ticks arrive bunched together, something between
-- here and there is buffering, and the size of the bunch is the delay.
--
-- That matters because for a gunshot, latency is not a detail - it is the
-- difference between a gunshot and a rumble that happens afterwards. This
-- makes it measurable during normal play instead of guessed at.
local TICK_INTERVAL = 0.25
local tickCount = 0
local lastTick = 0

local function Think()
    _G.PSVR2H_THINK_ALIVE = true
    pcall(inspectHand, 0)
    pcall(inspectHand, 1)
    pcall(pollWeapon)
    pcall(inspectDoors)

    local now = Time()
    if (now - lastTick) >= TICK_INTERVAL then
        lastTick = now
        tickCount = tickCount + 1
        emit(string.format("TICK:%d,%.3f", tickCount, now))
    end
    return PHYS_INTERVAL
end

-- Arming the physics sampler.
--
-- This used to run only from player_activate, which is fragile: the manifest
-- execs this script during level load, so depending on ordering (and on any
-- later script_reload_code) player_activate may already have fired and never
-- fire again. The result was the whole physics layer silently never starting -
-- no pickups, no throws, no impacts, for an entire session.
--
-- So arming is idempotent and gets retried from several entry points, including
-- a high-frequency event used purely as a heartbeat.
local function ensureThink()
    if _G.PSVR2H_THINK_ARMED then return end
    local player = safe(function() return Entities:GetLocalPlayer() end, nil)
    if player == nil then return end
    local ok = pcall(function()
        player:SetContextThink("psvr2_haptics_think", Think, 0.1)
    end)
    if ok then
        _G.PSVR2H_THINK_ARMED = true
        emit("PHYSICS_ARMED")
    end
end

--------------------------------------------------------------------------
-- Game event bindings
--------------------------------------------------------------------------

-- Hand assignment
local function onPrimaryHand(e)
    state.primaryIsLeft = fieldNum(e, "is_primary_left", 0) == 1
    emit("PRIMARY_HAND:" .. primarySide())
end
on("primary_hand_changed", onPrimaryHand)
on("single_controller_mode_changed", onPrimaryHand)
on("movement_hand_changed", function() emit("MOVEMENT_HAND") end)

-- Gravity gloves
--
-- These events carry "entindex" - the entity being pulled - which a field dump
-- of a real session revealed. That turns the signature interaction of the game
-- from inference into measurement: the object's actual mass, model and spin
-- are readable at the exact instant of the catch.
--
-- The old route waited for the physics sampler to notice whatever landed in
-- the hand shortly afterwards. That was both late and easy to misattribute -
-- catch two things in quick succession and the weights could swap. Reading the
-- entity the event names removes the ambiguity entirely.
local function gloveTarget(e)
    local index = fieldNum(e, "entindex", -1)
    if index < 0 then return nil end
    local ent = safe(function() return EntIndexToHScript(index) end, nil)
    if ent == nil then return nil end
    if not safe(function() return IsValidEntity(ent) end, false) then return nil end
    local mass = safe(function() return ent:GetMass() end, nil)
    if mass == nil then return nil end
    local model = safe(function() return ent:GetModelName() end, "")
    local cls = safe(function() return ent:GetClassname() end, "")
    return {
        mass = math.max(0.05, mass),
        material = materialFor(model, cls),
        model = model,
        spin = mag(safe(function() return GetPhysAngularVelocity(ent) end, Vector(0, 0, 0))),
    }
end

on("grabbity_glove_pull", function(e)
    local side = sideOfPrimaryFlag(e)
    local t = gloveTarget(e)
    if t == nil then
        emit("GLOVE_PULL:" .. side)
    else
        -- Mass on the PULL as well as the catch: dragging a hazmat crate
        -- across a room should not feel like reeling in a resin chip.
        emit(string.format("GLOVE_PULL:%s,%.2f,%s", side, t.mass, t.material))
    end
end)
on("grabbity_glove_locked_on_start", function(e) emit("GLOVE_LOCK_START:" .. sideOfPrimaryFlag(e)) end)
on("grabbity_glove_locked_on_stop", function(e) emit("GLOVE_LOCK_STOP:" .. sideOfPrimaryFlag(e)) end)
on("grabbity_glove_catch", function(e)
    local side = sideOfPrimaryFlag(e)
    -- The snap goes out immediately - latency is the whole point of a catch.
    emit("GLOVE_CATCH:" .. side)

    -- The weight of what actually arrived, read from the entity the event
    -- itself names. Lands inside the snap's own envelope, so it costs nothing.
    local t = gloveTarget(e)
    if t ~= nil then
        emit(string.format("GLOVE_CATCH_MASS:%.2f,%s,%s,%.1f,%s",
            t.mass, side, t.material, t.spin, sanitise(t.model)))
    else
        -- No usable entity on the event: fall back to the old route, where the
        -- physics sampler reports whatever lands in the hand next.
        armCatch(side)
    end
end)

-- Combat
on("player_shoot_weapon", function()
    state.roundsSinceReload = state.roundsSinceReload + 1
    -- Shells are individually chambered in Alyx, so this count is directly
    -- observable and reliable enough to mark the last one in the tube.
    -- -1 means "the game side cannot know", and the middleware treats that as
    -- "do not colour this shot". Only claim a count once a load has actually
    -- been observed.
    local remaining = -1
    if state.weapon == "SHOTGUN" and state.shellsKnown then
        state.shells = math.max(0, state.shells - 1)
        remaining = state.shells
    end
    emit("FIRE:" .. state.weapon .. "," .. primarySide() .. ","
        .. (state.twoHand and "2H" or "1H") .. "," .. tostring(remaining))
end)
on("player_hurt", function(e)
    -- Report how hard you were HIT, not how close to death you are.
    --
    -- The previous version scaled the haptic by remaining health, so identical
    -- hits felt different depending on a number displayed on your wrist. That
    -- is HUD information wearing a haptic costume - the same objection that
    -- got the kill cue removed. Alyx exposes no damage amount, but the health
    -- delta between events is a perfectly honest substitute.
    local health = fieldNum(e, "health", -1)
    if health < 0 then
        -- Health unavailable. Report the hit but do NOT invent a magnitude:
        -- the previous code fell back to health = 0, which turned every single
        -- injury into "you just took 100 damage" and fired the heaviest
        -- possible damage effect for a scratch.
        emit(string.format("HURT:-1,-1,%d", fieldNum(e, "damagebits", 0)))
        return
    end
    local damage = math.max(0, (state.lastHealth or 100) - health)
    state.lastHealth = health
    emit(string.format("HURT:%d,%d,%d", health, damage,
        fieldNum(e, "damagebits", 0)))
end)
-- entity_killed is deliberately NOT bound.
--
-- It fires for every entity that dies anywhere on the map, not just ones the
-- player killed, so it was buzzing the controller for distant NPCs. More
-- fundamentally, a kill is not something the hand physically feels - it is a
-- HUD cue wearing a haptic costume, and this project's rule is that if the
-- hand would not feel it, it does not vibrate.

-- Weapon identity
on("weapon_switch", function(e)
    local item = fieldStr(e, "item", "")
    -- Alyx frequently raises weapon_switch with an empty item string; the
    -- polled hand attachment is authoritative in that case, so stay quiet
    -- rather than reporting an unmapped empty token over and over.
    if item == "" then return end
    local w = classify(item)
    if w ~= nil then
        setWeapon(w, "switch")
    elseif not throttled("weaponraw:" .. item, Time(), 5.0) then
        emit("WEAPON_RAW:" .. sanitise(item))
    end
end)

-- Pistol
on("player_pistol_clip_inserted", function()
    setWeapon("PISTOL", "clip")
    state.roundsSinceReload = 0
    emit("PISTOL_CLIP")
end)
on("player_pistol_chambered_round", function() setWeapon("PISTOL", "chamber"); emit("PISTOL_CHAMBER") end)
on("player_retrieved_backpack_clip", function() emit("BACKPACK_RETRIEVE:" .. primarySide()) end)

-- Shotgun
on("player_shotgun_shell_loaded", function()
    setWeapon("SHOTGUN", "shell")
    state.shellsKnown = true
    state.shells = state.shells + 1
    emit("SHOTGUN_SHELL:" .. tostring(state.shells))
end)
on("player_shotgun_loaded_shells", function()
    setWeapon("SHOTGUN", "shells")
    -- Bulk load: the tube is full, so this is the natural resync point for a
    -- count that can otherwise drift.
    state.shellsKnown = true
    if state.shells < 1 then state.shells = 1 end
    emit("SHOTGUN_LOADED:" .. tostring(state.shells))
end)
on("player_shotgun_autoloader_state", function(e)
    -- Fires repeatedly even with no shotgun equipped; only report changes.
    local st = tostring(fieldNum(e, "state", 0))
    if state.autoloader == st then return end
    state.autoloader = st
    emit("SHOTGUN_AUTOLOADER:" .. st)
end)
on("player_shotgun_autoloader_shells_added", function() emit("SHOTGUN_AUTOLOAD_ADD") end)
on("player_shotgun_upgrade_grenade_launcher_state", function(e)
    emit("SHOTGUN_GL:" .. tostring(fieldNum(e, "state", 0)))
end)

-- Rapidfire (SMG)
on("player_rapidfire_cycled_capsule", function() setWeapon("SMG", "cycle"); emit("RAPID_CYCLE") end)
on("player_rapidfire_opened_casing", function() setWeapon("SMG", "open"); emit("RAPID_OPEN") end)
on("player_rapidfire_closed_casing", function() setWeapon("SMG", "close"); emit("RAPID_CLOSE") end)
on("player_rapidfire_inserted_capsule_in_chamber", function() emit("RAPID_INSERT") end)
on("player_rapidfire_inserted_capsule_in_magazine", function() emit("RAPID_MAG") end)
on("player_rapidfire_upgrade_fired", function() emit("RAPID_UPGRADE") end)
on("player_rapidfire_explode_button_pressed", function() emit("RAPID_EXPLODE") end)

-- Two-handed grips. These double as weapon identification.
-- Alyx repeats the two-hand grab events continuously while the grip is held or
-- released, not once per transition - observed firing dozens of times in a row.
-- Only an actual state change is worth a haptic.
local function twoHand(w, active)
    return function()
        -- setWeapon used to run HERE, before the state guard, and it wrecked
        -- the adaptive triggers.
        --
        -- Alyx raises two_hand_pistol_grab_start/end continuously, and it
        -- raises them even while a SHOTGUN is equipped. So every one of those
        -- spurious events reasserted weapon = PISTOL, the 60 Hz attachment
        -- poll immediately corrected it back to SHOTGUN, and the two fought
        -- forever: an observed session logged hundreds of PISTOL/SHOTGUN
        -- flips, each one rewriting the trigger's persistent profile.
        --
        -- The trigger motor never settled on a profile, which is precisely why
        -- every gun felt the same in the hand. Identity now changes only on a
        -- real transition, and only when nothing better is known - the polled
        -- hand attachment is ground truth and always wins.
        if state.twoHand == active then return end
        state.twoHand = active
        -- Only on a real transition, never on the repeat spam - but on a
        -- transition it IS authoritative, because the weapon is demonstrably
        -- in both hands at that moment.
        setWeapon(w, "2h")
        emit((active and "TWO_HAND_START:" or "TWO_HAND_END:") .. w)
    end
end
on("two_hand_pistol_grab_start", twoHand("PISTOL", true))
on("two_hand_pistol_grab_end", twoHand("PISTOL", false))
on("two_hand_rapidfire_grab_start", twoHand("SMG", true))
on("two_hand_rapidfire_grab_end", twoHand("SMG", false))
on("two_hand_shotgun_grab_start", twoHand("SHOTGUN", true))
on("two_hand_shotgun_grab_end", twoHand("SHOTGUN", false))

-- Items and inventory
-- item_pickup also carries item_name, which is more specific than item.
on("item_pickup", function(e)
    local item = fieldStr(e, "item", "")
    local name = fieldStr(e, "item_name", "")
    emit("ITEM_PICKUP:" .. sanitise(name ~= "" and name or item))
end)
on("item_released", function(e) emit("ITEM_RELEASE:" .. sanitise(fieldStr(e, "item", ""))) end)
on("player_picked_up_weapon_off_hand", function() emit("WEAPON_OFFHAND") end)
on("player_picked_up_weapon_off_hand_crafting", function() emit("WEAPON_OFFHAND") end)
on("player_stored_item_in_itemholder", function() emit("ITEM_STORE:" .. primarySide()) end)
on("player_removed_item_from_itemholder", function() emit("ITEM_REMOVE:" .. primarySide()) end)
on("player_drop_ammo_in_backpack", function() emit("BACKPACK_STORE:" .. primarySide()) end)
on("player_drop_resin_in_backpack", function() emit("BACKPACK_STORE:" .. primarySide()) end)

-- Health
on("player_health_pen_used", function() emit("HEALTH_PEN:" .. primarySide()) end)
on("player_using_healthstation", function() emit("HEALTH_STATION:" .. primarySide()) end)
on("health_pen_teach_storage", function() emit("HEALTH_STORAGE") end)
on("health_vial_teach_storage", function() emit("HEALTH_STORAGE") end)

-- World interaction
on("player_grabbed_by_barnacle", function() emit("BARNACLE") end)
on("player_released_by_barnacle", function() emit("BARNACLE_RELEASE") end)
on("tripmine_hack_started", function() emit("TRIPMINE_START") end)
on("tripmine_hacked", function() emit("TRIPMINE_HACKED") end)
on("player_started_2h_levitate", function() emit("LEVITATE") end)
on("player_covered_mouth", function() emit("COVER_MOUTH") end)
on("combine_tank_moved_by_player", function() emit("COMBINE_TANK") end)

-- Locomotion
--
-- Only the two cases where the hands are actually ON something survive here.
-- Mantling means gripping a ledge and hauling; grabbing a ladder means closing
-- a hand around a rung. Both are hand-contact events and both are kept.
--
-- JUMP and TELEPORT are gone. Neither involves the hands touching anything, and
-- generic locomotion vibration is exactly the kind of ambient buzzing that
-- masks the events that do matter.
on("player_continuous_mantle_finish", function() emit("MANTLE") end)
on("player_grabbed_ladder", function() emit("LADDER") end)
on("player_teleport_finish", function()
    -- Not emitted as a haptic - see above. Under continuous locomotion Alyx
    -- raises this on essentially every movement tick (thousands per session),
    -- which makes it a free heartbeat for arming the physics sampler if the
    -- normal entry points were missed.
    ensureThink()
end)

-- Session
on("game_newmap", function(e)
    emit("MAP:" .. sanitise(fieldStr(e, "mapname", "")))
end)
on("player_opened_game_menu", function() emit("MENU:1") end)
on("player_closed_game_menu", function() emit("MENU:0") end)

on("player_activate", function()
    -- A fresh map means a fresh player entity, so the previous arming is stale.
    _G.PSVR2H_THINK_ARMED = false
    -- ...and a stale health figure would make the first hit on a new map read
    -- as either enormous or nonexistent.
    state.lastHealth = 100
    state.shellsKnown = false
    state.shells = 0
    ensureThink()
    emit("READY:" .. VERSION)
end)
on("player_spawn", function() ensureThink() end)
on("game_newmap", function() _G.PSVR2H_THINK_ARMED = false end)

ensureThink()

emit("SCRIPT_LOADED:" .. VERSION .. ",listeners=" .. tostring(#handles))
