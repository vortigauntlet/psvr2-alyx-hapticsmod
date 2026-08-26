# Reference matrix

Which upstream project is authoritative for each subsystem, based on reading
their **source**, not their READMEs. Each row records what was verified and what
this project took from it.

> **Scope note.** This file records where each subsystem's *knowledge* came
> from. It is not a claim that any of it works — for what is actually verified,
> and at which of the five levels, see [VERIFIED.md](VERIFIED.md).

| Subsystem | Authoritative source | Status here |
|---|---|---|
| Event discovery | **bHaptics HLA** (`tactsuit.lua`) | Full inventory adopted; see the coverage note below |
| Middleware transport | **HalfLifeAlyxEventDetector** | Adopted — netcon implemented |
| Native hooks | **HLA-NoVR-DualSense** | Inspected, deliberately not adopted |
| VScript patterns | **AlyxLib** / **hla_extravaganza** | Partially adopted |
| Interaction tracking | **OpenGloves HLA scripts** | Ours is stronger |
| Adaptive triggers | *(none — see below)* | Ours is original |
| Waveform authoring | *(none — see below)* | Ours is original |
| Anti-repetition | **bHaptics Blade & Sorcery** | Adopted — continuous variation |
| Glove force-feedback | **OpenGloves driver** | Different actuator class, N/A |
| Physics state | *(none)* | Ours is original |

---

## Event discovery — bHaptics HLA

`scripts/game/hlvr/scripts/vscripts/tactsuit.lua` registers **59** game events
and is the only complete inventory of Alyx's haptics-relevant events.

Two field conventions in it are easy to get wrong and were both wrong in this
project before:

- `grabbity_glove_*` carries **`hand_is_primary`** (0/1), not a hand index.
  Reading `hand` silently yields the fallback, so every glove event lands on one
  hand.
- The fire event is **`player_shoot_weapon`**, not `weapon_fire`.

Coverage was verified mechanically against the full list.

**Coverage is deliberately no longer the goal.** Three bindings that bHaptics
has were subsequently *removed* here — `entity_killed`, `player_continuous_jump_finish`
and the teleport pair — because a vest and a pair of controllers are not the
same instrument. A vest can legitimately represent a kill or a jump somewhere on
the torso; a controller can only represent what the hand is touching. Matching
the reference project's event count would have meant breaking this project's
own rule, so the count went down on purpose. See the "deliberately not
implemented" table in [VERIFIED.md](VERIFIED.md).

## Middleware transport — HalfLifeAlyxEventDetector

The one project that does **not** tail `console.log`. It opens a TCP socket to
**127.0.0.1:29000** — Source 2's `-netconport` network console.

Wire format, read out of `HalfLifeAlyxGameMonitor.cs`:

```
receive   packets carry "PRNT" records; the printed line runs from the byte
          after the preceding NUL up to the newline
send      "CMND" 00 D3 00 00 00 | len byte (13 + strlen) | 00 00
          | lowercased UTF-8 command | 00
```

**Adopted.** `app/src/transport.cpp` implements both directions, with automatic
fallback to `console.log` so an existing `-condebug` setup keeps working.

Why it is better: no file buffering or flush latency, no unbounded log growth,
and it can push commands *into* the game — which makes live script iteration
possible (`script_reload_code`) without restarting.

## Native hooks — HLA-NoVR-DualSense

Inspected `dllmain.cpp` and `Enums.h` in full. It installs **four** AOB-scanned
inline trampolines:

| Value | Module | Signature scanned for |
|---|---|---|
| `weaponTypeN` | `server.dll` | `44 89 77 1C 4C 8B 74 24 28 …` |
| `pickupN` | `vphysics2.dll` | `FF 83 A0 2B 00 00 48 8B CE …` |
| `ClipSize` | `client.dll` | `89 9F B4 02 00 00 89 9F AC 01 …` |
| `HealthN` | `server.dll` | `8B B6 00 02 00 00 B8 00 00 00 00 …` |

Weapon enum confirmed: `TOOL = 73, SMG = 74, PISTOL = 78, SHOTGUN = 84`.

Two conclusions:

1. The vphysics2 hook sets a **pickup flag**. It is not a collision or impulse
   callback — anything claiming otherwise is wrong.
2. Of the four values, three (weapon type, pickup, health) are already available
   from VScript. The **only** unique gain is exact clip size.

**Not adopted**, deliberately: four hard-coded byte signatures that break on
every Alyx patch, in exchange for one number.

## VScript patterns — AlyxLib / hla_extravaganza

`alyxlib/controls/haptics.lua` (and its `hla_extravaganza` ancestor, both
derived from PeterSHollander's `glorious_gloves`) drives Alyx's **own** haptic
API, `FireHapticPulsePrecise(pulseWidth_us)`, with pulse width clamped to 1–30 µs.

That is a different channel from PSVR2Toolkit PCM and cannot carry waveforms, so
the API itself is not usable here. Two ideas from it are:

- **Squared strength curve** — `pulseStrength = pulseStrength * pulseStrength`
  before mapping to pulse width, a rough perceptual correction. This project
  does the equivalent more precisely with a measured response curve.
- **Pulse-train model** — a sequence is repeated pulses at an interval rather
  than one continuous effect. Adopted: material recipes now carry `pulses` and
  `pulseGapMs`, which is what makes cardboard read as a crumple.

## Interaction tracking — OpenGloves HLA scripts

`opengloves.lua` guards reload safety with a nil check:

```lua
if onitem_pickup_handle == nil then
    onitem_pickup_handle = ListenToGameEvent("item_pickup", OnItemPickup, nil)
end
```

This relies on globals surviving the reload and never unregisters. This project
uses the stronger form — persist handles on `_G`, then
`StopListeningToGameEvent` each one before re-registering — so a reload cannot
stack listeners even if the guard state is lost.

One detail worth taking: `item_pickup` also carries **`item_name`**, not just
`item`.

## Adaptive triggers — no authoritative source

**Awesome-Adaptive-Triggers** is a curated index, not an implementation. Its
repository contains `adaptivetriggersdb.txt`, `atignoredprocessesdb.txt` and a
README linking ~40 external tools (DSX, DualSenseY, PadForge, reWASD, duaLib…).
It contains **no** trigger parameter tables, curve presets or effect-construction
code.

The authoritative source for trigger semantics is therefore the toolkit's own
`pad_trigger_effect.h`, which this project follows directly, including the
documented ranges that are easy to violate silently:

- Weapon: `startPosition` 2–7, `endPosition` > start, max 8
- Slope: both strengths **1**–8 (zero is invalid, not "off")
- Multi-position feedback / vibration: ten independent values, 0–8

## Waveform authoring — no authoritative source

No inspected project authors PCM waveforms for this hardware. bHaptics projects
(including Blade & Sorcery) author `.tact` files, which are **spatial vest
patterns** — per-motor intensity over a body map — not audio-rate waveforms.
Useful as a model for *event → pattern* mapping, not for synthesis.

So the synthesis engine here is original, and is grounded in a measurement of
this specific hardware rather than borrowed values. See the response table in
`app/src/haptics.cpp`.

---

## Anti-repetition — bHaptics Blade & Sorcery

`TactsuitBS/bHaptics/` holds `.tact` files named `<Event>_1.tact`,
`<Event>_2.tact` … up to **five variants of a single event**
(`PlayerSpellIceRight` and `PlayerGunSprayRight` have five each), and the
project ships a `RandomNumber.cs` to pick between them.

That is a deliberate anti-repetition technique, and it is the one genuinely
transferable idea in any bHaptics project: replaying a byte-identical waveform
makes an effect read as canned no matter how well designed it is.

**Adopted, and generalised.** Because this project synthesises rather than plays
back files, it does not need discrete variants — `Router::Vary()` jitters every
instance continuously: pitch ±5.5%, level ±8%, length ±11%. Those amounts sit
far below the ~1.5x ratio needed for two pitches to read as different, so a hit
still lands unmistakably as glass or as stone; it just stops feeling like the
same recording each time.

The `.tact` format itself is not transferable — it is a spatial vest pattern
(per-motor intensity over a body map), not an audio-rate waveform.

## Glove force-feedback — OpenGloves driver

`server/src/services/input/input_force_feedback_named_pipe.cpp` receives
`ForceFeedbackCurlData` over a named pipe at
`\\.\pipe\vrapplication\ffb\curl\{left,right}`.

That is **per-finger curl limiting** for servo/brake gloves — a different
actuator class from a vibrotactile voice coil, with no waveform content at all.
Not applicable here. The named-pipe IPC pattern is a reasonable alternative
transport, but the Source 2 network console is strictly better for this use
because it also carries console output in.

## Coverage

Source inspected, all ten: bHaptics HLA, HLA-NoVR-DualSense, OpenGloves HLA
scripts, HalfLifeAlyxEventDetector, AlyxLib, hla_extravaganza,
Awesome-Adaptive-Triggers, HLA-NOVR-Mods, OpenGloves driver, bHaptics Blade &
Sorcery.
