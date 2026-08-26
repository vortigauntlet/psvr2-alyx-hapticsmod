# PSVR2 Alyx Haptics

Bespoke haptics and adaptive triggers for **Half-Life: Alyx** on **PSVR2 Sense
controllers**, via the PSVR2Toolkit CAPI.

No DLL injection. No pattern scanning. No patched Valve binaries.
Headset rumble is deliberately not used.

It does add one line to one Valve **text** file — see
[How the game side loads](#how-the-game-side-loads). That is reversible and
`Uninstall.bat` removes it.

```
Half-Life: Alyx
      |   VScript addon  (game/hlvr_addons/psvr2_haptics)
      v
  console.log
      |
      v
psvr2_alyx_haptics.exe
      |-- semantic event router -> Alyx-specific tactile profiles
      |-- adaptive trigger state machine (persistent base + transient overlays)
      '-- PCM voice synthesis and mixer (3 kHz, 32-sample chunks)
      v
PSVR2Toolkit CAPI  ->  PSVR2 Sense controllers
```

## Install

1. Add `-condebug` to Half-Life: Alyx's launch options
   (Steam → Library → Half-Life: Alyx → Properties → Launch Options).
   This is required: it is what makes the game write `console.log`.
2. Start SteamVR with the PSVR2 connected.
3. Run **Start Haptics.bat**.

That is the whole install. The executable finds Steam and Half-Life: Alyx by
itself, installs the game-side addon, and launches the game.

To remove it, run **Uninstall.bat**. It removes the addon folder, the script it
installed into the game's own `vscripts` folder, and the line it added to
`skill_manifest.cfg`.

## Checking it works before you play

**Test Haptics.bat** plays every tactile signature in turn with no game
running, so you can separate a hardware problem from a game-event problem from
a profile-design problem:

```bash
psvr2_alyx_haptics.exe --test glove-catch
```

`--probe` measures the PCM path and reports exactly what the driver is doing:

```bash
psvr2_alyx_haptics.exe --probe
```

`--analyze` needs no headset at all. It renders every signature through the
real mixer and prints what the waveform actually is — peak, RMS, duration,
dominant frequency and whether the limiter is squashing it:

```bash
psvr2_alyx_haptics.exe --analyze
```

That is what makes "is glass actually brighter than stone" a measurement
instead of an opinion, and it is the fastest way to catch a tactile regression
without launching the game.

## Tuning without a rebuild

Every waveform used to be a compile-time constant, which put a rebuild between
"that feels wrong" and trying something else. It doesn't any more:

```bash
psvr2_alyx_haptics.exe --dump-profiles
```

That writes `haptic_profiles.cfg` containing the values currently in use —
generated from the live table, so it can't drift out of step with the code the
way a hand-written example would. Edit a number, then `--test <name>` to feel
it or `--analyze` to measure it. No rebuild, no recompile.

```
[SHOTGUN_FIRE]
transient 200.0 0.340 16.0 10.0
body 160.0 45.0 0.890 520.0 300.0
```

The file is picked up automatically from beside the executable. Effects whose
shape is computed from live gameplay values — impact energy, caught mass, door
weight — stay in code, because there is nothing static to put in a file.

## Recording and replaying a session

```bash
psvr2_alyx_haptics.exe --record session.log
```

Play once, and that session becomes a repeatable test case with **real** masses,
materials and firing cadence. Then:

```bash
psvr2_alyx_haptics.exe --replay session.log --analyze
```

`--replay` alone plays it back on hardware with the original timing. Adding
`--analyze` renders it offline with no headset at all and reports what it
produced — which is the only way to catch the one problem no self-test can,
because self-tests fire one effect at a time and real play **overlaps** them.

That is not hypothetical: replaying a firefight is what revealed repeated
shotgun fire summing with its own ring-out and driving the limiter down, which
is now fixed by choking each shot with the next.

## Command line

| Option | Effect |
|---|---|
| *(none)* | install/update the addon, then run |
| `--launch` | also start Half-Life: Alyx through Steam |
| `--test [name]` | play tactile signatures without the game |
| `--list-tests` | list the test names |
| `--version` | print the version and exit |
| `--dump-profiles` | write the current tactile profiles to an editable file |
| `--profiles <f>` | load tactile profiles from a specific file |
| `--record <f>` | log every semantic event to a replayable session |
| `--replay <f>` | play a session back (add `--analyze` for offline) |
| `--probe` | measure the PCM path and report driver behaviour |
| `--analyze` | render every signature offline and print peak/duration/pitch — no hardware needed |
| `--sweep` | frequency response check — which frequencies you actually feel |
| `--hands` | left/right localisation check |
| `--install` / `--uninstall` | manage the game addon only |
| `--no-install` | leave the game addon alone |
| `--debug` | verbose trigger/PCM/impact/event diagnostics |

## How the game side loads

The addon installs to:

```
Half-Life Alyx\game\hlvr_addons\psvr2_haptics\scripts\vscripts\
    game\gameinit.lua              <- the engine runs this automatically
    mods\init\psvr2_haptics.lua
    psvr2_haptics\core.lua
```

Half-Life: Alyx automatically executes `scripts/vscripts/game/gameinit.lua`
from the addon search path for **every** map, including the base campaign.
Our `gameinit.lua` follows the community
[Scalable Init Support](https://gitlab.com/recursivenomad/scalable_init_support)
convention: it enumerates every enabled addon and runs each one's own init
file, so whichever addon wins the search-path collision, every mod still loads.

**Two install routes are written, and only one of them currently works.**

Route 2 above — the tidy `hlvr_addons` layout — is the one that *should* work,
and it is written on every install so that it will if the addon is ever
published to the Workshop and enabled. But Half-Life: Alyx only mounts addons
that are **enabled**, and a hand-installed folder never appears in
`default_enabled_addons_list`. The engine log says so plainly: every level load
reports `addons []`, and no script output is ever produced.

So route 1 is what actually runs today:

```
Half-Life Alyx\game\hlvr\scripts\vscripts\psvr2_haptics.lua
Half-Life Alyx\game\hlvr\cfg\skill_manifest.cfg   <- one line appended
```

The appended line is `script_reload_code psvr2_haptics.lua`. This is the same
mechanism the bHaptics Alyx integration uses.

Two honest consequences:

- **A Valve text file is modified.** Nothing is patched or injected and no
  binary is touched, but `skill_manifest.cfg` is not left alone. `--uninstall`
  removes the line.
- **A game update that restores Valve's original `skill_manifest.cfg` will
  silently switch the haptics off.** The install re-applies itself on every
  run, so the fix is to start through **Start Haptics.bat** as usual — but if
  haptics stop after an Alyx patch, this is the first thing to suspect.

## Tactile design

### The one rule

> **If the player's hand would not physically feel it, do not vibrate for it.**

This is VR. The hands are real, tracked, and holding things. A haptic that does
not correspond to something a hand is touching is not immersion, it is noise —
and worse, it raises the noise floor that every honest effect has to compete
against.

So the following are **deliberately absent**, not missing:

| Not implemented | Why |
|---|---|
| Footsteps, ambient buzz | No hand contact. |
| Jump, teleport | No hand contact. Generic locomotion vibration. |
| Kill confirmation | A HUD cue in haptic clothing. Nothing is felt. (It also fired for *every* entity dying anywhere on the map.) |
| Impacts of thrown objects | Your hand let go. It is not connected to that collision. |
| Directional damage with no hand relationship | Nothing physically reaches the palm. |
| Headset rumble | Out of scope for this project. |

`MANTLE` and `LADDER` are kept, because gripping a ledge and closing a hand
around a rung are genuine hand-contact events.

Full verification status — including what has and has not been felt on real
hardware — is in [docs/VERIFIED.md](docs/VERIFIED.md).

### Architecture

**Adaptive triggers carry persistent state; PCM carries transient detail.**

A weapon installs a base trigger profile that stays until the weapon changes.
Momentary events push a time-limited *overlay* on top, and the base restores
itself when the overlay expires — so a reload click can never permanently wipe
the shotgun's trigger feel.

Weapons are differentiated by using genuinely different trigger **modes**, not
by nudging one mode's numbers:

| Weapon | Mode | Feel |
|---|---|---|
| Pistol | multi-position feedback | light take-up, firm wall, crisp break |
| Shotgun | multi-position feedback | heavy throughout, breaks much later |
| SMG | feedback | light single stage, comfortable for repeat fire |
| Grenade | slope feedback | continuously rising tension as you wind up |
| Multitool | feedback | light mid-travel detent |

Two caveats worth stating rather than glossing over. The **grenade** profile is
unlikely to be reached in the base campaign — grenades are physics props, not
trigger weapons, so they will not raise `player_shoot_weapon`; they reach
haptics through the *throw* path instead. The **melee** profile in the source
is likewise unreachable: Alyx has no crowbar weapon. Both are kept for mods,
and neither should be described as working.

PCM effects are built from layered voices — a bright transient for the edge, a
mid-band body for weight, band-passed noise for material texture, and sometimes
a resonant tail — rather than one oscillator.

### Measured actuator response

The usable band is **not** set by Nyquist. It was measured on real hardware
with `--sweep` — fifteen equal-amplitude tones, 40 Hz to 800 Hz:

| Band | Response |
|---|---|
| 40–60 Hz | fairly strong (low lobe) |
| 80–110 Hz | **weak — a real dip** |
| 120–180 Hz | very strong |
| **180–300 Hz** | **peak** |
| 300–500 Hz | fairly strong |
| 600 Hz+ | not felt at all |

Two things follow:

- **The ceiling is ~520 Hz.** An earlier revision put glass's transient at
  720 Hz, its texture at 950 Hz and its resonance at 620 Hz. Every layer that
  made glass *glass* was above the cutoff, so it collapsed to its body layer —
  and so did metal, plastic and most reload clicks. That is why everything felt
  the same. All design frequencies now live inside 40–500 Hz.
- **The response is far from flat**, so a designed amplitude did not mean what
  it said. `ResponseGain()` inverts the measured curve per sample, which also
  lets a heavy impact sweep *through* the 80–110 Hz dip without hollowing out.

### Separating materials in a narrow band

Vibrotactile pitch discrimination is coarse — roughly a 1.5× ratio is needed
before two frequencies read as different — so 40–500 Hz yields only about
**five** reliable pitch slots (~55, ~130, ~200, ~310, ~470 Hz). That is not
enough for eight materials.

So every material is separated on **at least two** axes, and the extra axes are
ones skin resolves *better* than pitch — duration, pitch movement, and temporal
pattern:

| Material | Pitch | Length | Movement | Modulation | Reads as |
|---|---|---|---|---|---|
| glass | 470 Hz | 200 ms | slight fall | fast shimmer | tinkling |
| plastic | 470 Hz | 110 ms | none | none | sharp clack |
| metal | 310 Hz | 520 ms | none | slow pulse | ringing |
| wood | 200 Hz | 200 ms | slight fall | none | solid knock |
| cardboard | 160 Hz | 4 hits / 400 ms | none | none | progressive crush |
| organic | 125 Hz | 250 ms | none | slow wobble | squish |
| stone | 190→50 Hz | 520 ms | huge fall | none | boom |
| rubber | 115 Hz | 90 ms | none | none | dead thud |

Glass and plastic deliberately *share* a pitch: one shimmers and lasts twice as
long, the other is a bare clack. That contrast is far more legible than the
third-of-an-octave gap they previously had.

The gravity gloves use pitch movement as their signature — the pull sweeps
**up** 85→430 Hz over 440 ms, the catch drops **down** 470→210 Hz. Nothing else
in the game moves like either.

The catch is then given its **weight** separately. No VScript API exposes the
glove's target, so the mass of what you caught cannot be known at the moment
the catch fires. Instead the snap plays immediately — latency is the whole
point of a catch — and the object is identified when it lands in the hand a
tick or two later, arriving as `GLOVE_CATCH_MASS` and layering a low settle
underneath, inside the snap's own envelope.

The snap is deliberately bright and short so the two layer instead of fighting
for the same band. Measured, a 0.4 kg bottle and a 22 kg crate land **2.4×
apart in pitch and 1.9× apart in duration** (231 Hz / 268 ms against
96 Hz / 502 ms). If those two ever converge, the signature interaction of the
game has regressed to a single canned buzz — and `--analyze` will show it.

They are separated by **rhythm** as well, and that is the axis carrying the
light end. Something heavy loads, sags and settles — a slow wobble. Something
light has no authority to plant itself: it clatters into the palm and is still
moving when it gets there, which is a fast shimmer. Catching something nearly
weightless genuinely *is* almost the bare capture, so pitch and duration alone
were never going to separate those two.

Levels are set by a gain-riding limiter rather than a fixed saturator. A static
one has to be tuned either for the loudest case, leaving quiet events tiny, or
for the quiet case, squashing loud ones flat — either way everything converges
on the same perceived level.

To re-measure on different hardware, run `--sweep` and update the response
table at the top of `app/src/haptics.cpp`. The sweep's own tones deliberately
bypass both the clamp and the compensation, so it measures the actuator rather
than measuring our correction.

### Campaign moments

The events Half-Life: Alyx actually raises are a fixed list, so coverage here is
a question of doing justice to what exists rather than of adding more. Several
of the most memorable hand-contact beats in the game were being rendered as a
single oscillator lasting 42–66 ms, which the hardware round had already shown
is too little time for skin to integrate into anything at all.

Each is now built from the layers the moment physically has:

| Moment | What the hand is doing | How it is built |
|---|---|---|
| **Covering your mouth** (Jeff) | palm pressed to your own face | no transient at all — the only effect here with none. A soft low presence that swells over 90 ms and then breathes. Deliberately quiet: it plays while you are holding your breath. |
| **Barnacle** | grabbing at a tongue hauling you up | wet contact, a coiling grip that tightens, and a **rising** haul underneath — rising is what makes it read as being lifted rather than hit |
| **Health station** | hand pushed into a machine | mechanism engaging, then the needle 230 ms later, then warmth spreading. The wait is what makes it a machine deciding rather than a button pressed. |
| **Combine barrier** | heaving with your whole body | low, rough and long, in both hands |
| **Two-handed levitate** | lifting something too heavy for one hand | a slow swell in both hands, with the roughness of a load that does not want to move |
| **Mantling** | hauling your own weight over a ledge | grip bites, then the load comes on and rides |
| **Tripmine hack** | reaching into the beam | rising tension on start, a hard release when it gives |
| **Bracing a weapon** | support hand onto the foregrip | arrival that resolves *downward* into stillness — what bracing changes is that the thing stops moving |

Storing and retrieving deserve a note. They are the same gesture in opposite
directions and you perform both constantly, which makes them the pair most worth
separating. Direction of pitch alone did not do it. What does is what the hand
is actually doing: putting something away is a push that **ends in a seat**, so
it takes time and stops; pulling something out is a yank that is over the moment
the object clears.

## Weapon and ammunition state

Alyx never hands VScript a clean weapon enum, so weapon identity is assembled
from three independent signals, strongest last:

1. the `item` string on `weapon_switch`
2. the classname/model attached to the primary hand, polled at ~60 Hz
3. weapon-specific manipulation events — a shotgun shell can only be loaded
   into a shotgun, so these are the most reliable

Anything unrecognised is reported as `WEAPON_RAW` in debug rather than guessed
at, so profiles can be extended against real data.

Shotgun shells are counted from load and fire events, which makes the last
shell in the tube feel different and adds a dry-click when it runs empty. This
is **inferred, not read** — the game does not expose a magazine count — so it
can drift, resyncs on a bulk load, and only ever *adds* a cue to a shot that
already happened. It can never suppress one.

## Physics

The rule the whole physics layer is built around:

> **If the player's hand would not physically feel it, do not vibrate for it.**

The game side samples held objects at ~60 Hz: mass, linear velocity, angular
velocity, hand velocity, model and classname.

An impact is reported **only for an object that is still in your hand.** Once a
prop leaves the hand, nothing about its collision is felt, because your hand is
no longer connected to it — a bottle smashing three metres away is not
something your palm can know about.

This is also how melee works. **Half-Life: Alyx has no swingable melee
weapon**, so hitting something means bashing it with a grabbed prop, which is
exactly the held-impact path. There is no separate melee system.

A held prop follows the hand, so it decelerates whenever the hand does. The
discriminator is the *difference* — in a real collision the object sheds speed
the hand did not:

```
excess  = objectSpeedLost - handSpeedLost
impulse = mass × excess
```

`impulse ≈ mass × Δvelocity` is a **heuristic, not a real contact impulse** —
it ignores the collision normal and any energy that went into rotation. VScript
gets no collision callback, so this is an inference and is reported as one:
every impact carries a **confidence** value, and a low-confidence reading plays
softer rather than being either suppressed or asserted at full strength.

Material class is likewise inferred from model/classname, because Alyx does not
expose surface properties to VScript.

The thresholds are unvalidated guesses and expect tuning. `debug=true` prints
every impact with its impulse, mass, spin and confidence for that purpose.

## Extra: the Options button and the Alyx menu

Unrelated to haptics, but fixed here for convenience.

Half-Life: Alyx ships SteamVR bindings for Index, Touch, Vive, Cosmos and WMR —
but **none for `playstation_vr2_sense`**. SteamVR therefore auto-remaps Alyx's
Oculus Touch binding onto the Sense controllers.

In that binding the menu action sits on `/user/hand/left/input/y`, which the
PSVR2 driver maps to **left Triangle**. The driver *does* map Touch's
`application_menu` to the **right Options** button — but Alyx never binds
`application_menu` at all, so that mapping leads nowhere and Options does
nothing.

`tools/fix_options_button.py` adds one source to the active binding:

```
/user/hand/right/input/options  ->  /actions/dev/in/togglemenu
```

```bash
python tools\fix_options_button.py
```

It is additive (left Triangle keeps working), idempotent, and backs the file up
first. `--revert` restores the backup.

The binding it edits is SteamVR's own per-app autosave, stored as a Workshop
item. If SteamVR ever regenerates it, the Options button will stop working
again — just re-run the script.

## Configuration

`psvr2_haptics.cfg` is entirely optional; see the comments in the file. It
supports master and per-event and per-weapon gains, handedness, physics
thresholds and `debug=true`. UTF-8 BOM is tolerated.

## Building

```bash
powershell -ExecutionPolicy Bypass -File .\Package.ps1
```

Requires MSVC and CMake ≥ 3.20. The game-side Lua is compiled into the
executable, so the release is a single self-installing binary.

## Toolkit compatibility

The PSVR2Toolkit CAPI is explicitly work-in-progress, and the DLL shipped with
the PlayStation VR2 App does not always match upstream's source. Two real
differences are handled at runtime rather than assumed:

- `psvr2_toolkit_wait_for_pcm` returns **1** on the shipped build, not `0`.
  Upstream returns `PSVR2TK_RESULT_OK` (0). Comparing against 0 makes every
  wait look like a failure and **silently starves the entire PCM path** — which
  is exactly what happened in earlier versions of this project, leaving only
  adaptive-trigger resistance detectable.
- `psvr2_toolkit_write_pcm` returns an uninitialised register value on the
  shipped build, consistent with a `void`-returning function.

`Capi::Calibrate()` measures both at startup and adapts, so a future toolkit
that returns proper codes is picked up automatically and gets full error
reporting.

## Credits and references

- [PSVR2Toolkit](https://github.com/BnuuySolutions/PSVR2Toolkit) — the CAPI this
  builds on.
- [bHaptics Half-Life: Alyx integration](https://github.com/bhaptics/bhaptics-half-life-alyx)
  — reference for Alyx game-event names and field keys.
- [Scalable Init Support](https://gitlab.com/recursivenomad/scalable_init_support)
  — the addon auto-load convention.
