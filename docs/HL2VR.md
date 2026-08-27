# Half-Life 2 VR integration

The same PSVR2 haptics core that drives Half-Life: Alyx, with a second game
adapter in front of it.

> **Status.** The middleware half is complete, compiles clean and is measurable
> today with Half-Life 2 VR *not installed*.
>
> There are **two** routes into the game, and the first needs nothing built:
>
> * **bHaptics WebSocket** — Half-Life 2 VR ships with bHaptics support built
>   in, which means it already broadcasts its own semantic haptic events to
>   `127.0.0.1:15881`. We can simply be the thing that answers. Protocol
>   verified; the game's actual event names still need one play session to
>   discover, which is what `--bhaptics-scan` is for.
> * **Source server plugin** — richer (mass, surface material, spin) but
>   **has never been compiled or run**.
>
> The boundary is drawn explicitly in
> [Runtime validation required](#runtime-validation-required).

---

## Architecture

```
                       Half-Life: Alyx              Half-Life 2 VR
                            |                             |
                    VScript addon              built-in bHaptics support
                    (embedded in the exe)      + optional server plugin
                            |                             |
              netcon 29000 / console.log       ws 15881 / udp 29001
                            |                             |
                    +-------+-------+             +-------+-------+
                    | games/alyx    |             | games/hl2vr   |
                    | AlyxAdapter   |             | Hl2vrAdapter  |
                    +-------+-------+             +-------+-------+
                            |                             |
                            +--------------+--------------+
                                           |
                                    IGameAdapter
                                           |
        +----------------------------------+----------------------------------+
        |                              core/                                   |
        |  events   materials   impact   variation   profiles                  |
        |  haptics (voices, mixer, limiter)      triggers (state machine)      |
        |  transport      config      hmd                                      |
        +----------------------------------+----------------------------------+
                                           |
                                    core/capi  ->  PSVR2Toolkit CAPI
                                           |
                                 PSVR2 Sense controllers
```

Nothing in `core/` knows which game is running. `main.cpp` names a game in
exactly one function, `MakeAdapter`, and everything else — recording, replay,
analysis, the trigger scheduler, the CLI — runs against `IGameAdapter`.

---

## Route 1: the game's own bHaptics stream (needs nothing installed)

**Half-Life 2 VR ships with bHaptics support built in.** The Steam page lists
it, and bHaptics' own setup guide for the game is: install Half-Life 2, install
the VR mod, press play. There is no mod to download, because the Source VR Mod
Team implemented it themselves.

That matters because of *how* bHaptics integrations talk to hardware. Both of
bHaptics' SDKs — the C# one and the native C++ `haptic-library`, whose
third-party dependency list is nlohmann/json and **easywsclient** — are
WebSocket **clients**. They connect out to the bHaptics Player at:

```
ws://127.0.0.1:15881/v2/feedbacks?app_id=<id>&app_name=<name>
```

and send JSON:

```json
{ "Register": [ { "Key": "<name>", "Project": {...} } ],
  "Submit":   [ { "type": "key", "key": "<name>" } ] }
```

`Register` announces **every haptic pattern the game owns, on connect**.
`Submit` fires one by name.

So a game with built-in bHaptics support is already broadcasting its own
semantic event vocabulary to a local port — classified by the people who wrote
the game. On a PSVR2 machine the bHaptics Player is not running, so that port is
free, and this middleware can simply answer.

**What that buys over reading game state:**

* Nothing to build or install. No SDK, no 32-bit toolchain, no plugin.
* **No inference.** "The game says the shotgun fired", not "the magazine count
  went down, so probably a shot".
* No injection, no offsets, no hooks. We never touch the game; it connects to
  us.

**What it does not buy** — and why the plugin still has a purpose: a bHaptics
submit carries an event *name* and vest motor data. It does not carry the mass
of what you caught, the surface property of what you hit, or how fast it was
spinning. The material system needs those. The two routes are complementary.

### The one thing that needs the game

The transport is verified. The **key names** are not, because reading them needs
Half-Life 2 VR running. So nothing here guesses them:

```bash
psvr2_alyx_haptics.exe --game hl2vr --bhaptics-scan
```

listens, prints every pattern the game announces and every one it fires, says
which rule matched and why, and does nothing else. One play session turns the
list from unknown into known — at which point the substring rules in
`games/hl2vr/hl2vr_bhaptics.cpp` should be replaced with exact matches.

Until then the mapper matches on lowercase substrings, and it **refuses to
guess**: an unmapped key produces no haptic and one line naming it. A wrong
mapping would be worse than silence — it would fire the wrong sensation at the
right moment, which is much harder to notice than nothing happening.

It also drops, deliberately, anything a vest can represent and a controller
cannot: heartbeat, breathing, footsteps, vehicle rides. That is this project's
golden rule applied to somebody else's event list, and taking all of them would
be the fastest possible route to the ambient buzzing this whole project exists
to avoid.

### You do not need bHaptics hardware. That is the point.

Worth stating plainly, because it reads backwards at first: **not owning
bHaptics is what makes this route work.** The bHaptics Player is the thing that
normally owns port 15881. With no bHaptics software installed, the port is free,
Half-Life 2 VR connects to us instead, and we render its events on PSVR2.

If you *do* have the bHaptics Player running, it takes the port first and this
route is unavailable — the middleware detects that case and says so rather than
failing silently.

### What was ported from bHaptics, and what was deliberately left

Their Half-Life integration has 98 distinct effects. Audited against this
project's golden rule — *if the hand would not physically feel it, it does not
vibrate* — they fall into three groups.

**Already covered**: shooting per weapon, recoil kickback, chambering, clip
insertion, the gravity-glove verbs, health pen and charger, grenade launch,
explosion damage.

**Ported, because it was a real gap**: bHaptics splits damage by TYPE, and this
project had one generic damage shape. Burning, being shocked and being poisoned
genuinely do not feel like being shot, and all four arrive in the arms:

| type | dur | pitch | what carries it |
|---|---:|---:|---|
| shock | 100 ms | 389 Hz | the sharpest, brightest, shortest thing in the game |
| damage (bullet) | 208 ms | 125 Hz | one hard jolt |
| explosion | 343 ms | 84 Hz | the concussive slam |
| fire | 562 ms | 183 Hz | **no transient at all** — nothing struck you |
| toxic | 671 ms | 97 Hz | the longest and lowest: a state, not an impact |

`ShockOnHand` ported directly too — one of the few entries in their whole list
that is unambiguously a *hand* sensation. It reuses the shock waveform on one
hand instead of both, because the sensation is identical and only the location
differs.

**Deliberately left**: `Combine`, `MetroPolice`, `Sniper`, `Turret`, `Strider`,
`RollerMine` and the entire `Unarmed<enemy>` family — twenty-odd effects
encoding *who hit you* as a direction across the torso. A vest can say that; two
controllers cannot, and the hand feels the same jolt either way. Also dropped:
`HeartBeat`, `Cough`, and every `*Head` variant.

Taking all of them would have added twenty effects that measure the same, which
is the exact failure this project has now found four times.

### Proven without the game

The listener, the JSON scan, the key mapper and the adapter were tested end to
end against a simulated bHaptics client that speaks the real protocol
(`fake_hl2vr_bhaptics.py`). The handshake is checked against RFC 6455's
published test vector in `--verify`, because a subtly wrong handshake fails as
"the game never connects" — which is indistinguishable from "the game does not
use this transport", and that ambiguity would be expensive.

---

## Route 2: a server plugin, and why not source modification

The brief asked for a source-level integration wherever one was possible. For
Half-Life 2 VR it is not, and the reason is worth recording:

* **The HL2VR source is not public.** The Source VR Mod Team's
  [FAQ](https://halflife2vr.com/faq/) says the mod "can, in principle, be open
  sourced" under the Source SDK 2013 terms, but that they are doing due
  diligence first and there is "currently no definitive timeline". The only
  public HL2VR repository,
  [vittorioromeo/HL2VRU](https://github.com/vittorioromeo/HL2VRU) ("Half-Life 2:
  VR Mod - Unleashed"), distributes binaries — its git tree contains only
  `.gitignore` and `README.md`.
* **There is no scripting layer.** Source SDK 2013 ships VScript headers but no
  usable VM, and stock Half-Life 2 has no `logic_script`. HL2VR is not
  Mapbase-derived.

What remains is the engine's own documented extension point: a **server plugin**
implementing `ISERVERPLUGINCALLBACKS003`, loaded from `hl2\addons\*.vdf`. It is
an engine feature rather than a game feature, so it needs none of HL2VR's
source. No injection, no pattern scanning, no hardcoded offsets — the standing
rule on this project, and here also simply the only maintainable option.

Field offsets are resolved **by name** at load from the server DLL's own network
tables (`IServerGameDLL::GetAllServerClasses`). A game update that moves a field
is followed automatically; one that removes a field fails loudly at load naming
the field. There is no gamedata file to maintain.

---

## What Half-Life 2 can and cannot tell us

This shapes every design decision below, so it comes first.

Every `CreateEvent()` reachable from singleplayer in Source SDK 2013 was read.
The complete useful set is `player_hurt` (which carries **no damage amount**),
`entity_killed`, `break_breakable` (which carries a **material**), `break_prop`,
`physgun_pickup`, `weapon_equipped`, `ammo_pickup`, `take_health` and
`take_armor`.

There is **no** `weapon_fire`, no reload event, no melee event and no explosion
event. `door_moving` exists but is `#ifdef CSTRIKE_DLL`.

So most events are derived from polled state — a magazine count falling is a
shot, a held object's velocity collapsing is an impact. That is the same
technique the Alyx addon already uses for held impacts, and it is why nearly
every event carries a confidence the middleware uses to play a doubtful reading
softer rather than either suppressing it or asserting it at full strength.

Full per-event table: [`src/games/hl2vr/plugin/README.md`](../app/src/games/hl2vr/plugin/README.md).

---

## The material system

Half-Life 2 is in a much better position here than Alyx. Alyx does not expose
surface properties to VScript, so its game side guesses the material from the
model path. Source 1 exposes the real thing:

```
IPhysicsSurfaceProps::GetPropName()   ->  "wood_crate", "glassbottle", ...
surfacedata_t::game.material          ->  CHAR_TEX_WOOD 'W', CHAR_TEX_METAL 'M'
                                              |
                                     hl2vr_surfaces.cpp
                                              |
                                     generic Material class
                                              |
                                     core/materials.cpp recipe
```

The prop **name** is preferred because it distinguishes things the character
code cannot — rubber and cardboard have no code of their own — and the
character code is the reliable fallback for a prop nobody has classified. Both
travel with every event that has a surface.

`break_breakable` carries a third form, a `BREAK_*` flag, mapped separately.

One class was added to the core for Half-Life 2: **Dirt**, for loose ground.
Alyx never produces it, so adding it could not change any Alyx signature — and
the measurement is what decided its shape. The intuitive "soft low thud" put it
at 145 Hz, inside the discrimination threshold of both rubber and organic; loose
ground is really a long granular *hiss*, which is a free cell and a better
description.

---

## Weapons

Half-Life 2 has **seven** firing weapons where Alyx had three, so the placement
problem is genuinely harder and cannot be solved by nudging numbers. Every pair
must be separable, because the game hands you all of them at once.

### Waveforms — measured, not intended

| weapon | dur | pitch | what carries it |
|---|---:|---:|---|
| SMG | 65 ms | 214 Hz | the shortest thing in the game; a burst must read as a burst |
| crossbow | 95 ms | 430 Hz | the only shot with **no** low-frequency energy at all |
| pistol | 112 ms | 268 Hz | the middle rung, and the weapon fired most |
| grenade | 150 ms | 165 Hz | a throw, not a discharge |
| AR2 | 197 ms | 314 Hz | fast tremolo: electrical, not mechanical |
| magnum | 257 ms | 152 Hz | a violent crack rather than a boom |
| shotgun | 504 ms | 87 Hz | the long fall into the low lobe |
| RPG | 622 ms | 145 Hz | the only shot whose pitch **rises** |

The magnum deliberately does not go lowest. A .357 is higher pressure and
shorter than a shotgun; putting it where "most powerful handgun" suggests
collided with both the shotgun and the RPG, and moving it up is both more
accurate and what buys the low end its room.

### Adaptive triggers

Differentiation comes from using genuinely different trigger **modes**, which
are differences in kind, and only then from laddering within a mode:

| weapon | resting profile | kick rate | reveal | reads as |
|---|---|---:|---:|---|
| crossbow | MultiFeedback, near-immovable | 14 Hz | 60 ms | a drawn bow held at tension |
| shotgun | MultiFeedback, heavy and rising | 16 Hz | 260 ms | a long pull that never snaps |
| magnum | Weapon, late and hard | 12 Hz | 150 ms | one deliberate heavy break |
| pistol | Weapon, early and light | 26 Hz | 60 ms | a crisp break |
| AR2 | Slope, rising | 34 Hz | 140 ms | a weapon that spools up |
| SMG | Feedback, flat and light | 38 Hz | 300 ms | held down for seconds |
| RPG | Feedback, mid | 20 Hz | 420 ms | a plain trigger on a tube |
| gravity gun | Feedback, almost nothing | — | — | the load comes from the **object** |
| crowbar | Feedback, deep | — | — | a grip, not a trigger |

Recoil is two stages, as the Alyx recoil bench established: the trigger goes
**slack** first (the break), and the kick is revealed underneath. The reveal
window is the whole sensation and is guarded in code.

The gravity gun's near-zero resting profile is deliberate. Its trigger is a
grip, and what it should communicate is what is in the beam — a heavy resting
profile would sit underneath the mass overlay and flatten the entire range.

---

## The gravity gun

The signature weapon, and the one given the most deliberate treatment.

* **Grab** — a bright snap, then the weight arrives *underneath* it in a
  separate band. Split for the same reason Alyx splits its catch: capture and
  mass in one band fight each other and the limiter flattens a heavy grab back
  toward a light one.
* **Hold** — a 4 Hz heartbeat. The trigger holds a static load proportional to
  mass, capped at 5 of 8 (Guerrilla's finger-fatigue finding applies squarely to
  a load held for as long as you carry something). The PCM adds inertia **only
  while the object is actually moving** — standing still holding a crate
  produces no waveform at all, which is the gate that keeps this from becoming
  ambient buzzing.
* **Launch** — the trigger snaps free and the object departs. Mass moves it on
  both length and pitch, so a soda can and a filing cabinet are different
  events, not the same event at different volumes.
* **Punt** — a shove at something *not* held. No load to release, so no break
  and no relief: a short bright push with the struck material answering back.
* **Drop** — the load simply ends.
* **Supercharged** — a real state change the game drives from
  `physcannon_mega_enabled`, so it is read rather than guessed. The weapon comes
  alive in the hand: the longest and brightest thing it ever does.

**There is no charge-up haptic, because Half-Life 2's gravity gun has no charge
state.** The brief listed one as an example; inventing it would be exactly the
fabricated physics this project refuses.

---

## The crowbar

The most important rule in the whole integration:

> **A swing through empty air does not vibrate.**

Your hand feels the weight of the bar, not a buzz, and a cue on every swing
turns the most-used tool in the game into the noise floor everything else
competes against. So the swing only *arms* the window in which an impact will be
believed, and loads the trigger as the grip tightens. If nothing is struck,
nothing is felt.

`--verify` asserts this: `melee-swing` must render **silent**, and the check
fails if it ever stops being.

Impacts are material-dependent, and heavier than the same material as a dropped
prop — you are driving a steel bar into it with your whole arm.

---

## Developing with the game not installed

Everything below runs today, with no headset and no Half-Life 2 VR.

Run the offline self-checks — the event model, the codec, the materials, and
that every self-test case renders or is asserted silent:

```bash
psvr2_alyx_haptics.exe --game hl2vr --verify
```

Render every signature and measure what it actually produces, then report any
pair a hand could not tell apart:

```bash
psvr2_alyx_haptics.exe --game hl2vr --analyze
```

Listen for the game's own bHaptics events (needs the game *running*, but
nothing installed and no headset):

```bash
psvr2_alyx_haptics.exe --game hl2vr --bhaptics-scan
```

List the synthetic test cases:

```bash
psvr2_alyx_haptics.exe --game hl2vr --list-tests
```

Play one signature on real hardware, if a PSVR2 is connected:

```bash
psvr2_alyx_haptics.exe --game hl2vr --test grav-launch-heavy
```

Replay a recorded or hand-written session offline. A session file is
`<ms>\t<EVENT>:<params>` and can be typed by hand:

```bash
psvr2_alyx_haptics.exe --game hl2vr --replay session.txt --analyze
```

Synthetic events go through `Handle()` — the *same* entry point the live plugin
drives. There is no parallel test path that could drift from the real one.

### Adding an event

1. Add a case to `Hl2vrAdapter::Handle` in `games/hl2vr/hl2vr_adapter.cpp`.
2. Add a name to `SelfTestNames()` and a line to `RunSelfTest()`.
3. Put it in a family in `AnalyzeFamily()` — or return `nullptr` and say why.
4. Run `--game hl2vr --analyze` and fix any collision it reports.

Step 3 is not optional. Three times now this project has found the same silent
failure — one waveform with the pitch nudged — and every time it was in the
layer that had no test.

### Adding a haptic profile

Fixed-shape effects live in `core/profiles.cpp` and are overridable from a text
file at runtime, so they can be retuned without a rebuild:

```bash
psvr2_alyx_haptics.exe --dump-profiles
```

writes the current built-ins in exactly the format the override file takes.

---

## Installing, once you have Half-Life 2 VR

1. Build the plugin — see
   [`src/games/hl2vr/plugin/README.md`](../app/src/games/hl2vr/plugin/README.md).
   It needs the Source SDK 2013 **singleplayer** branch and a **32-bit**
   toolchain.
2. Put `psvr2_haptics_plugin.dll` next to `psvr2_alyx_haptics.exe`.
3. `psvr2_alyx_haptics.exe --game hl2vr --install` — writes
   `hl2\addons\psvr2_haptics.vdf` and copies the DLL beside it. If the DLL is
   missing it says so plainly rather than reporting a successful install of
   nothing.
4. Start the game, open the console, and run `plugin_print`. `psvr2_haptics`
   should be listed.
5. `psvr2_alyx_haptics.exe --game hl2vr` and play.

Debugging: `psvr2_haptics_console 1` in the game console prints every event as
it happens; `--debug` on the middleware prints what it did with each one.

---

## Runtime validation required

Nothing on the game side has been compiled or run. In rough order of how likely
each is to need work:

1. **Does Half-Life 2 VR actually use the bHaptics WebSocket?** Run
   `--game hl2vr --bhaptics-scan`, turn bHaptics on in the game's options, and
   play. If keys appear, route 1 works and most of the plugin's risk evaporates.
   If nothing ever connects, the tool says so plainly rather than assuming, and
   the plugin becomes the only route.
2. **What are the real key names?** The scan prints them. Send me the list and
   the substring rules become exact matches.
3. **Does Half-Life 2 VR load server plugins at all?** Only matters for route 2.
   `plugin_load addons/psvr2_haptics_plugin` is the fastest test.
4. **Do the seven network properties resolve?** The load message prints
   `resolved N/7`; anything less names the field that failed.
5. **Is entity index 1 the player?** True for stock singleplayer Source.
6. **Does `physgun_pickup` fire for HL2VR's VR grab?** HL2VR replaced the flat
   game's pickup interaction. If its grab does not route through
   `Pickup_OnPhysGunPickup`, the entire gravity-gun family goes quiet and needs
   another signal. This is the largest single risk to the feature set.
7. **Are the two impact thresholds anywhere near right?** They are starting
   points, not measurements. Set `psvr2_haptics_debug 1`, carry and swing a
   crate, a barrel and a can, then read the candidates back with `--impacts`.
8. **Does the melee trace agree with what the player actually hit?** It runs
   from the server's idea of the eye and view direction; in VR the crowbar is
   swung by a *hand* this plugin cannot see. Fallbacks are documented in the
   plugin README.
9. **Does the shotgun increment its clip one shell at a time in HL2VR?** It does
   in stock Half-Life 2; HL2VR may have changed the reload.
10. **Which hand is which.** Half-Life 2 VR is a two-handed VR mod, but nothing
   this plugin can read says which hand holds the weapon. Everything currently
   goes to the configured primary hand. If HL2VR exposes hand state in any
   network table, the `hand=` field is already in the wire format and the
   adapter already honours it.
11. **Then, and only then, the feel.** Every number in the tables above is a
   measurement of the waveform, not of the sensation. `--analyze` proves two
   effects are *different*; only hands prove they are *right*.
