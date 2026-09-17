# Half-Life 2 VR game-side plugin

The Half-Life 2 counterpart of the Alyx VScript addon: it classifies gameplay
and sends semantic events to the middleware. It touches no hardware.

**This code has not been compiled and has never been run.** It is written
against the published Source SDK 2013 headers, and every interface, event and
field it uses is cited below against the file it was read from. That is a
different and much weaker claim than "it works", and the difference is the
whole point of this document.

---

## Why a plugin at all

Half-Life: Alyx ships a scripting layer, so its integration is a VScript addon
that prints tagged console lines. Half-Life 2 has no equivalent:

* Source SDK 2013 carries VScript *headers* (`public/vscript/ivscript.h`) but no
  usable VM, and stock Half-Life 2 exposes no `logic_script` entity. HL2VR is
  not a Mapbase-derived mod, so there is nothing to write a script in.
* **The HL2VR source is not public.** The Source VR Mod Team's
  [FAQ](https://halflife2vr.com/faq/) says the mod "can, in principle, be open
  sourced" under the Source SDK 2013 terms but has not been, and gives no
  timeline. The only public HL2VR repository,
  [vittorioromeo/HL2VRU](https://github.com/vittorioromeo/HL2VRU), ships
  binaries and a README — the repository contains no source at all (checked via
  the GitHub API: the tree is `.gitignore` and `README.md`). So direct source
  modification is not available.

What *is* available is the engine's documented third-party extension point: a
**server plugin**. It is an engine feature rather than a game feature, so it
needs none of HL2VR's source; it is loaded from a `.vdf`; and it gets the game
event manager plus the public vphysics and engine-trace interfaces. No
injection, no pattern scanning, no hardcoded offsets.

---

## What it is built on, and where each thing was verified

Everything below was read out of
[ValveSoftware/source-sdk-2013](https://github.com/ValveSoftware/source-sdk-2013).

| Used | Verified in |
|---|---|
| `IServerPluginCallbacks`, `ISERVERPLUGINCALLBACKS003` | `public/engine/iserverplugin.h` |
| `IGameEventManager2` (`GAMEEVENTSMANAGER002`), `IGameEventListener2` | `public/igameevents.h` |
| `IServerGameDLL::GetAllServerClasses` | `public/eiface.h` |
| `ServerClass` / `SendTable` / `SendProp` walking | `public/server_class.h`, `public/dt_send.h` |
| `IServerTools::GetKeyValue`, `GetBaseEntityByEntIndex` | `public/toolframework/itoolentity.h` |
| `IPhysics::GetActiveEnvironmentByIndex`, `IPhysicsEnvironment::GetObjectList` | `public/vphysics_interface.h` |
| `IPhysicsObject::GetMass/GetVelocity/GetMaterialIndex/GetGameData` | `public/vphysics_interface.h` |
| `IPhysicsSurfaceProps::GetPropName/GetSurfaceData` | `public/vphysics_interface.h` |
| `IEngineTrace::TraceRay` | `public/engine/IEngineTrace.h` |
| `CHAR_TEX_*` material codes | `game/shared/decals.h` |
| `BREAK_GLASS/METAL/FLESH/WOOD/CONCRETE` | `public/const.h` |
| `m_iClip1`, `m_flNextPrimaryAttack` are SendProps | `game/shared/basecombatweapon_shared.cpp` |
| `m_hActiveWeapon` is a SendProp | `game/server/basecombatcharacter.cpp` |
| Crowbar range 75, hit/miss both set the same refire delay | `game/server/basebludgeonweapon.cpp` |
| Supercharged gravity gun is convar-driven | `game/server/hl2/weapon_physcannon.cpp` |

### No offsets, anywhere

`psvr2_sendprops.cpp` resolves every field **by name** at load, by walking the
server DLL's own network tables. A game update that moves a field is followed
automatically; one that removes a field fails loudly at load naming the field,
instead of silently reading a neighbouring member. There is no gamedata file.

---

## What Half-Life 2 actually tells us

Every `CreateEvent()` reachable from singleplayer was read. The complete useful
set is:

| Event | Fields | Used for |
|---|---|---|
| `player_hurt` | userid, health, attacker — **no damage amount** | not used; damage is the polled health delta |
| `entity_killed` | entindex_killed/attacker/inflictor, damagebits | deliberately unused (a kill is not a hand sensation) |
| `break_breakable` | userid, entindex, **material** | breaking something, with its material |
| `break_prop` | userid, entindex | breaking something, no material |
| `physgun_pickup` | entindex | gravity gun **and** `+USE` pickups |
| `weapon_equipped` | class, entindex, owner_entindex | acquiring a weapon |
| `ammo_pickup` | ammo_index, amount, total | picking up ammunition |
| `take_health` / `take_armor` | — | health kits and armour |

There is **no** `weapon_fire`, no reload event, no melee event and no explosion
event. `door_moving` exists but is `#ifdef CSTRIKE_DLL`, so it never fires here.

Everything else is therefore derived from polled state:

| Event | How it is derived | Confidence |
|---|---|---|
| `HL2_FIRE` | `m_iClip1` falls | High — a reload can only raise the count |
| `HL2_RELOAD` / `HL2_SHELL` | `m_iClip1` rises (by one, for the shotgun) | High |
| `HL2_DRYFIRE` | refire timer advances with an empty magazine | High |
| `HL2_MELEE_SWING` | refire timer advances on the crowbar | High |
| `HL2_MELEE_HIT` | a trace re-run the way the weapon runs it | **Approximate — see below** |
| `HL2_GRAV_GRAB` / `HL2_CARRY_GRAB` | `physgun_pickup` + the weapon in hand | High |
| `HL2_GRAV_LAUNCH` / `HL2_GRAV_PUNT` | primary fire with / without something held | Medium |
| `HL2_IMPACT` | a held object's speed dropping abruptly | Thresholded, and reports its own confidence |
| `HL2_DAMAGE` | health + armour delta | High; the amount is a delta, not a reported figure |
| `HL2_MEGA` | `physcannon_mega_enabled` | High — this is how the game decides |

### What this plugin CANNOT see, and the bHaptics route can

Half-Life 2 VR's manual reload is a sequence of physical VR actions - eject the
magazine, catch it, reach over your shoulder, insert it, chamber a round - and
the shotgun pump is two distinct off-hand motions. **None of that is visible
here.** Those are HL2VR's own VR interaction state, which lives in its
non-public code and appears in no network table this plugin can read.

What the plugin sees is the CONSEQUENCE: `m_iClip1` changing. From that it can
say "a reload completed" and, for the shotgun, "one more shell went in". It
cannot say which hand did it or which step just happened.

So the two routes carry genuinely different information:

| | plugin | bHaptics stream |
|---|---|---|
| which weapon fired | yes | yes |
| rounds remaining | yes | no |
| mass / surface / spin | **yes** | no |
| damage TYPE (fire, shock, toxic) | no | **yes** |
| manual reload STEPS | no | **probably** |
| which hand | no | **probably** (Left/Right suffixes) |

Neither supersedes the other, and running both is better than either. That is
why the middleware accepts them simultaneously rather than picking one.

### The melee trace is the weakest thing here

`HL2_MELEE_HIT` re-runs the crowbar's own trace (75 units forward,
`MASK_SHOT_HULL`) and reports the surface it finds. In flat Half-Life 2 that
would agree with the game closely. **In VR the crowbar is swung by a hand**, and
this plugin cannot see where that hand is — HL2VR's VR state is in its own
non-public code and is not in any network table this can read.

So the trace runs from the server's idea of the player's eye and view
direction. It will be right when the player swings roughly where they are
looking and wrong when they do not. This is the single largest item needing
runtime validation, and if it proves bad the honest fallbacks are, in order:

1. report a hit only when `break_breakable` or a nearby physics object's
   velocity confirms one — accurate, but silent against walls;
2. drop `HL2_MELEE_HIT` entirely and keep only the swing, which arms the
   trigger and plays nothing.

Both are better than a crowbar that buzzes at the wrong moments.

---

## Building

Needs the Source SDK 2013 **singleplayer** branch (32-bit; the `master` branch
is now 64-bit and will not load into HL2VR):

```bash
git clone -b singleplayer https://github.com/ValveSoftware/source-sdk-2013
```

```bash
cmake -S . -B build -A Win32 -DSOURCESDK=/path/to/source-sdk-2013/sp/src
```

```bash
cmake --build build --config Release
```

Put the resulting `psvr2_haptics_plugin.dll` next to `psvr2_alyx_haptics.exe`
and run `psvr2_alyx_haptics.exe --game hl2vr --install`, which copies it into
`<Half-Life 2 VR>\hl2\addons\` alongside the `.vdf` that loads it.

Expect to fix things on the first build. Include paths and the exact set of
`tier1` symbols vary between SDK branches, and the CMakeLists is a starting
point written from the headers rather than a recipe that has been run.

### Checking it loaded

In the game console:

```
plugin_print
```

`psvr2_haptics` should be listed. Then `psvr2_haptics_console 1` (the default)
prints every event as it happens, so the integration can be watched without
attaching anything.

---

## Runtime validation required

Nothing in this directory has been run. In rough order of how likely each is to
need work:

1. **Does HL2VR load server plugins at all?** Everything else depends on it.
   `plugin_load addons/psvr2_haptics_plugin` in the console is the fastest test.
2. **Do the seven network properties resolve?** The load message prints
   `resolved N/7`; anything less names the field that failed.
3. **Is entity index 1 the player?** True for stock singleplayer Source. If
   HL2VR spawns differently, `LocalPlayerEntity()` needs adjusting.
4. **Does `physgun_pickup` fire for HL2VR's VR grab?** HL2VR replaced the flat
   game's pickup interaction; if its grab does not route through
   `Pickup_OnPhysGunPickup`, the whole gravity-gun family goes quiet and needs
   another signal.
5. **Are the two impact thresholds anywhere near right?** They are starting
   points. Set `psvr2_haptics_debug 1`, carry and swing a crate, a barrel and a
   can, then read the `HL2_CANDIDATE` lines back with `--impacts`.
6. **Does the melee trace agree with what the player hit?** See above.
7. **Does the shotgun really increment `m_iClip1` one shell at a time in
   HL2VR?** It does in stock Half-Life 2; HL2VR may have changed the reload.
8. **Does the AR2 secondary produce a usable two-stage signal?** The charge
   events are not emitted yet for exactly this reason — see the note in
   `psvr2_plugin.cpp`.
