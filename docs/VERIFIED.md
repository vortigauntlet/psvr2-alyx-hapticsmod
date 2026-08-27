# Verification matrix

What is actually true about this build, at five distinct levels of proof.

| Level | What it means |
|---|---|
| **CODE** | The source exists. Nothing more is claimed. |
| **STATIC** | The full producer → transformation → consumer path was read end to end and connects. |
| **RUNTIME** | Executed and measured on this machine, without a headset. |
| **HARDWARE** | Felt on real PSVR2 Sense controllers. |
| **NOT IMPL** | Does not exist. Listed so its absence is on the record. |

A row is only marked at a level if that level was actually reached. **HARDWARE is
the level that matters, and most of this build has not reached it yet** — the
tactile design work in this revision has been measured, not felt.

Last updated: 2026-08-27. Game script `VERSION = "7.0"`.

---

## How the runtime column was established

Not by inspection. By running things:

| Check | Command | Result |
|---|---|---|
| Compiles clean | `cmake --build … --config Release` | MSVC `/W4 /permissive-`, zero warnings |
| Game script is valid Lua | `luaparser` over `addon/**/*.lua` | 3/3 parse |
| Every signature renders | `--analyze` | 51 signatures, peak/rms/duration/dominant-Hz/limiter |
| Measurements are reproducible | `--analyze` twice | Byte-identical output. Previously they were NOT: per-instance variation drew from one RNG stream across the whole suite, so inserting a test case moved every later row by up to 11%. |
| No perceptual collisions | `--analyze` | 0 pairs inside 1.5x on both axes, across 9 families |
| No orphaned events | script cross-reference | 54 matched, 0 genuinely unhandled |
| Tests are order-independent | `--analyze` | Each case resets weapon identity and rate limits. Weapon *gain* used to leak between cases, so a glove catch measured differently depending on whether a shotgun test ran before it. |
| Addon deploys | `--install` + read back from disk | `VERSION = "7.0"` present in the game folder |

`--analyze` is the important one. It renders each effect through the real mixer
and reports what the waveform *is*, so "glass is brighter than stone" is a
measurement rather than an opinion. It needs no hardware, which is what makes
iterating on tactile design possible at all.

---

## Transport and hardware layer

| Feature | Level | Notes |
|---|---|---|
| CAPI binding (6 exports by name) | **STATIC** | `capi.cpp`. Load fails cleanly on a missing export rather than calling a bad pointer. |
| PCM write path | **HARDWARE** | Previously reported working after the ABI fix. |
| `wait_for_pcm` ABI variance | **HARDWARE** | Shipped DLL signals ready with `1`, not `0`. `WaitIsReady(rc >= 0)` accepts both. Comparing against `0` starved PCM entirely — the original root cause. |
| `write_pcm` returns void | **RUNTIME** | `Calibrate()` measures it per run instead of hard-coding either ABI. Return value ignored unless a build proves it meaningful. |
| Adaptive triggers | **HARDWARE** | `rc=0`, and increased resistance was physically felt. |
| Network console (`-netconport`) | **STATIC** | Implemented, preferred over log tailing. **Not confirmed connected in-game.** |
| `console.log` tailing (`-condebug`) | **HARDWARE** | The route currently in use. |
| Headset rumble | **NOT IMPL** | Deliberately never bound. Out of scope by instruction. |

---

## Synthesis and mixing

| Feature | Level | Notes |
|---|---|---|
| 4-bus mixer (Transient/Body/Texture/Sustain) | **RUNTIME** | Measured via `--analyze`. |
| Transient sidechain ducking | **STATIC** | A live transient ducks texture and sustain so the edge stays legible. |
| Gain-riding limiter | **RUNTIME** | Per controller. Lowest in the suite is 0.80 (`shotgun`). |
| Voice stealing (28 voices/hand) | **STATIC** | Lowest priority then oldest. |
| Measured actuator response curve | **HARDWARE** | From `--sweep`: peak 180–300 Hz, dip 80–110 Hz, nothing above ~520 Hz. Every oscillator is clamped to that ceiling. |
| Per-instance variation | **RUNTIME** | ±5.5% pitch, ±8% level, ±11% length — below the ~1.5× ratio needed to change perceived pitch, so identity survives. |
| Persistent trigger base + overlay stack | **STATIC** | Priority-ordered, auto-restoring. A reload click can no longer wipe the shotgun profile. |
| Held trigger state (`ClearOverlays`) | **CODE** | New. Used for the glove lock detent. Not yet felt. |
| Refreshable overlays (`RefreshOverlay`) | **CODE** | For states reported as a heartbeat. Updating in place is silent; clear-and-repush would make the trigger toggle audibly every beat. |
| **Weapon-fire choke group** | **RUNTIME** | New. A shot fades its predecessor instead of summing with it. Found by offline session replay: repeated fire drove the limiter to 0.53 and the first casualty was the transient. |
| **Editable profiles** | **RUNTIME** | New. Waveforms load from a text file. Verified end to end: editing `SHOTGUN_FIRE` moved it from 94 Hz/542 ms to 294 Hz/187 ms **with no rebuild**. |

### Measured response of the adaptive trigger motor — **HARDWARE VERIFIED**

Taken with `--trigger-sweep`, one variable at a time, trigger held half-pressed
so there was travel in both directions. This is the trigger's equivalent of the
grip actuator's frequency table, and until it existed **every trigger value in
this project was reasoned about by analogy rather than measured.**

| Axis | Result |
|---|---|
| **Frequency** | 10–30 Hz reads as genuine **kickback**. 40–50 Hz starts becoming vibration. Past 60 Hz it is *only* vibration. **Best ≈ 20 Hz.** |
| **Strength** | Clearly distinct across the **whole 0–8 range** — unlike the grip actuator it does not plateau, so amplitude is a real design axis. |
| **Mode** | `Feedback`, `Weapon` and `Slope` all genuinely **resist the finger**. **`Slope` resists hardest.** Vibration modes do not resist, they shake. |
| **Position** | A wall only exists **ahead of the finger**. Feedback-from-start and Feedback-halfway feel clearly different. |

Two designs died on these facts before they were known:

1. Recoil frequencies sat at 24–130 Hz — mostly above the kickback band, which
   is why the SMG at 130 Hz was reported as "not moving with it" despite firing
   correctly with `rc=0` on every shot.
2. The "shove" was a `Feedback` wall parked at position 1, *behind* a finger
   holding the trigger down. There was nothing there to push against, which is
   why the shotgun's full-strength kick registered as nothing.

An intermediate round also inverted the frequency rule based on a three-weapon
comparison where mode, position, strength and duration all differed at once —
nothing could be attributed. The controlled sweep reversed it.

### Recoil is set by CYCLE COUNT, not milliseconds — **HARDWARE VERIFIED**

The single most useful measurement in the whole trigger effort, and it came
from one observation: at 90 ms / 14 Hz the shotgun "still feels like two
bursts", where 60 ms / 14 Hz read as one.

That pins the boundary exactly. 1.26 cycles = two pulses. 0.84 = one. So a
single-pulse weapon should land **just under 1.0 cycle** — the most energy the
pulse can carry without starting a second.

One cycle is `1000 / rate` ms, so the target moves with the rate rather than
being a fixed duration:

| weapon | rate | one cycle | reveal | cycles | reads as |
|---|---|---|---|---|---|
| pistol | 21 Hz | 47.6 ms | **46 ms** | 0.97 | one crisp snap |
| shotgun | 14 Hz | 71.4 ms | **69 ms** | 0.97 | one heavy shove |
| smg | 30 Hz | 33.3 ms | **200 ms** | 6.0 | sustained rattle |

This corrected two live values: the shotgun sat at 80 ms = 1.12 cycles (a full
pulse plus a stub of a second — the same stutter disliked at 1.26, just
smaller), and the pistol at 40 ms = 0.84 (a pulse cut off before completing).

It also replaced an earlier rule that said every effect needs **≥1.5 cycles to
be felt**. That rule came from wanting a vibration to read *as a vibration* and
is simply wrong for an impulse: for a punch, more cycles is the defect.

**Weapons are separated by pulse COUNT** — one / one / many — which is a
categorical difference rather than a matter of degree, and is the kind skin
reads best. Pistol and shotgun are both single pulses, told apart by rate
(21 vs 14 Hz), drive (6 vs 8) and duration (1.5×).

### Resting pull and recoil are separate axes

Conflated for several revisions, which is why the shotgun kept reading as
"heavy" in the wrong way — a stiff trigger rather than a hard shove.

| weapon | resting pull | recoil kick |
|---|---|---|
| grenade | 4/8 | *none — a throw is not a discharge* |
| pistol | 5/8 | 6/8 |
| smg | 6/8 | 7/8 |
| shotgun | **7/8** | **8/8** |

The shotgun is deliberately **not** the stiffest trigger to hold — only one
notch above the pistol — but it is the one that hits back hardest. Shots inside
700 ms ease its resting profile by a further notch, on the reasoning that
working a pump twice in a row does not take the same effort the second time.

### The trigger at full depth — **HARDWARE VERIFIED**

Measured with `--deep-test`, trigger held hard against its stop. Prompted by a
precise observation: at half depth every weapon's recoil pushed back, but
bottomed out the shotgun and SMG stopped pushing and only vibrated into the
controller body — while the pistol still kicked. The pistol was the only one
not at maximum drive.

| Question | Answer |
|---|---|
| Does drive strength change it? | **Yes — 6/7 pushes back BETTER than 8/8.** At full drive the motor stalls against the stop and its force goes into the body, not the finger. |
| Does rate change it? | Yes. **16 Hz and 24 Hz measured best.** |
| Can anything shove the trigger back up from the stop? | **No.** Feedback at 0 and 2, Slope across the low third, a narrow Weapon band and MultiFeedback loaded low — none of them lift it. |

Three consequences, all of them counter to how this file was written:

1. **Never drive recoil vibration at 8/8 on a trigger that is held bottomed.**
   Maximum strength is *weaker* in the hand than 7, because the motor stalls
   against the stop and its force goes into the body instead of the finger.

   **Correction, 2026-08-27.** Earlier revisions of this file claimed a
   universal cap enforced by a constant named `kMaxKickStrength`. That symbol
   has never existed in the source, and the cap is not universal. What the code
   actually implements is `kMaxSustainedDrive = 7` (`router.cpp`), applied to
   **sustained fire only** — which today means the SMG.

   The narrower rule is the better one, and the code's own note explains why:
   the stall was measured with the trigger held hard against its stop, and the
   SMG is the only weapon fired that way. A pistol or shotgun trigger is pulled
   and released, so it spends its travel in a region where the motor has room
   to work and full drive is not wasted. The shotgun therefore still uses 8
   deliberately, and `--analyze` reports it.

   Recorded here rather than quietly amended, because a documented safeguard
   that does not exist is worse than no safeguard: it stops anyone looking.
2. **Weight comes from rate and length**, never the last notch of strength.
3. **At full depth, recoil can only ever be vibration.** No resistance mode can
   create an impulse there. Any design that tries to shove at the stop is
   wasting its effort, and a weapon that is held bottomed should take its punch
   from the grip actuator instead.

### Which layer is the kick — **HARDWARE VERIFIED**

Six candidate recoil designs played back to back (`--recoil-lab`), because three
consecutive single-guess redesigns had all been reported as feeling like
nothing. Results:

| | design | verdict |
|---|---|---|
| **F** | Weapon 260 ms **+ 16 Hz vibration** | **best** |
| E | Feedback 8, 260 ms | ok |
| A | Slope, 90 ms | better than B |
| B | Slope, 260 ms | worse than A |
| C / D | Weapon alone, 90 / 260 ms | **felt like nothing** |

Two conclusions, both by direct elimination:

1. **The vibration is the kick.** F and D are identical apart from the 16 Hz
   layer; D registered as nothing and F was the winner. Resistance modes are a
   *load* for the kick to push against, not the impulse. Every previous design
   had this backwards — it sized the resistance as the kick and treated the
   vibration as decoration.
2. **Short resistance beats long.** A (90 ms) beat B (260 ms) in the same mode,
   which disproves the "the motor needs travel time" theory that had just been
   used to justify lengthening everything.

Recoil is therefore built as **resistance load → low-frequency kick**, with the
weapon ladder living in the kick layer:

| | load | kick | total |
|---|---|---|---|
| pistol | Weapon 3-6 @5, 90 ms | 20 Hz @5, 130 ms | 130 ms |
| smg | Weapon 3-6 @6, 55 ms *per round* | 18 Hz ramp, 110 ms | chains |
| shotgun | **Weapon 2-7 @8, 260 ms** | **16 Hz @8, 300 ms** | **300 ms** |

Each kick outlasts its load, so the load expiring *reveals* the kick still
running — that reveal is what makes the pair read as a single gesture.

Untested and worth an A/B later: `Feedback` outscored `Weapon` as a standalone
load (ok vs nothing), so `Feedback + vibration` may beat the winning
`Weapon + vibration`. Not changed on that inference alone, since F is the only
combination actually felt.

### PCM byte encoding — **HARDWARE VERIFIED**

Long-standing untested assumption, settled with `--pcm-format`: the toolkit
takes **signed** two's-complement samples. Unsigned-centred-on-128 was clearly
*weaker* on hardware. The output stage is correct, and persistent reports of
"weak" are not caused by a format error.

### Hardware feedback round 1 — what it taught

Seven signatures were reported too weak on real hardware: `shotgun-empty`,
`reload`, `shell-insert`, `shotgun-pump`, `glove-lock`, `smg`, `catch-light`.
`impact-wood` and `impact-stone` were reported as good. Lining those up against
the measurements gave two clear failure modes, and **neither was amplitude** —
every one of them peaked at 0.53–0.83, the same as the effects that worked.

**1. Short clicks carry no energy.** `reload`, `shell-insert` and
`shotgun-pump` all measured 27–34 ms at rms 0.03 — about a tenth of
`impact-wood` (rms 0.112, 209 ms). Peak is instantaneous; what skin integrates
is level over time. All ten reload events also shared one shape with only the
pitch changed, which is the same "nudge the numbers of one mode" failure this
project criticises elsewhere in weapon triggers.

**2. High frequencies cannot carry force.** `glove-lock` at 468 Hz, `smg` at
418 Hz and `catch-light` at 340 Hz were the three weak ones that were *not*
short. `ResponseGain()` compensates by up to 2.1×, but amplitude compensation
cannot make a voice coil displace further at high frequency — the energy has to
move down the band, not up in level.

The resulting rule, now applied throughout:

> **Character goes in the accent, at any frequency. FORCE goes at 150–300 Hz.**

A bright tick is punctuation and costs almost nothing; the low body underneath
is what is actually felt. Two-stage mechanisms (a slide racking, a pump
cycling) additionally exploit the fact that skin resolves *timing* far better
than pitch.

| signature | rms before → after | dur before → after | domHz before → after |
|---|---|---|---|
| `reload` | 0.031 → **0.094** | 34 → 118 ms | 262 → 195 |
| `shell-insert` | 0.027 → **0.105** | 27 → 124 ms | 181 → 153 |
| `shotgun-pump` | 0.029 → **0.149** | 30 → 254 ms | 200 → 154 |
| `glove-lock` | 0.085 → 0.091 | 134 → 141 ms | 468 → **289** |
| `smg` | 0.101 → 0.113 | 136 → 131 ms | 418 → **335** |
| `catch-light` | 0.140 → 0.119 | 188 → 189 ms | 340 → **248** |

`glove-lock`, `smg` and `catch-light` barely moved in rms **on purpose** — rms
is electrical amplitude, not felt force. Dropping 468 → 289 Hz roughly doubles
displacement at the same amplitude. `catch-light`'s rms even fell, because in
an efficient part of the band you need *less* drive, not the same.

Open question for the next round: the response table in `haptics.cpp` may still
understate the high-frequency rolloff, since it was built from a coarse
subjective sweep. It has **not** been changed — the architectural fix was the
safer move, and altering that table would shift every effect at once.

### Which effects are bilateral, and why

Four signatures deliberately reach **both** hands. This was invisible until
`--analyze` grew a `hands` column, and it caused a false bug report: the
self-test announced the hand it *asked* for, then a bilateral effect buzzed
both, which looks exactly like a routing bug. The test now reports where the
effect actually went.

| Signature | Why both |
|---|---|
| `hurt`, `hurt-light` | Damage is a whole-body event. Your brief rules out *directional* damage — "arbitrary left-side enemy damage makes left controller buzz despite no hand interaction" — so inventing a side would be worse than using neither. |
| `impact-heavy`, `impact-stone` | A genuinely heavy object (mass > 5 kg, near-full energy) loads the opposite hand through the body. Light impacts stay on one hand. |
| `shotgun` | The test case fires it **two-handed** (`2H`). The support hand feels the frame, duller and quieter than the firing hand. `shotgun-empty` uses `1H` and is correctly one-handed. |

`hold-still` reports `trig` — trigger load with no waveform at all. That is the
measured proof the grip load does not degenerate into ambient buzzing.

### Tooling added this revision

| Tool | What it is for |
|---|---|
| `--dump-profiles` | Writes the live values as an editable file, so tuning needs no rebuild — and no hand-written example can drift from the code. |
| `--record` / `--replay` | The brief's replay requirement. Play once; that session becomes a repeatable test with real masses and cadence. |
| `--replay --analyze` | Renders a recording offline, no hardware. The only way to see **overlap**, which every one-effect-at-a-time test is blind to. |
| Delivery monitor | The game script emits `TICK` every 250 ms of game time; the middleware compares observed spacing against intended spacing to detect transport buffering. Latency was previously uninstrumented entirely. |
| `select()` on the network console | The poll interval no longer contributes to latency on the preferred transport. |

### Measured signature set

All 29 render distinctly. Dominant frequency spans 90–468 Hz — effectively the
entire band the hardware can produce.

Measured **with the shipped config**, not with defaults. That distinction
mattered: the config's legacy `gain.GLOVE_CATCH=1.15` boost, stacked on levels
that had already been rebalanced, drove a heavy catch into the limiter at 0.65
and squashed the very contrast the mass layer exists to create. Both glove
gains are now 1.00. Nothing in the suite limits below 0.80.

| | peak | rms | dur (ms) | dom Hz |
|---|---|---|---|---|
| shotgun | 0.99 | 0.403 | 558 | 86 |
| pistol | 0.96 | 0.169 | 187 | 267 |
| smg | 0.98 | 0.109 | 90 | 299 |
| glove-lock | 0.57 | 0.049 | 83 | 325 |
| glove-pull | 0.94 | 0.268 | 475 | 191 |
| glove-catch | 0.95 | 0.133 | 176 | 255 |
| **catch-light** | 0.95 | 0.143 | 268 | 231 |
| **catch-heavy** | 0.96 | 0.189 | 502 | 96 |
| **hold-still** | 0.00 | 0.000 | **0** | — |
| hold-swing | 0.43 | 0.075 | 258 | 77 |
| impact-glass | 0.99 | 0.137 | 233 | 429 |
| impact-metal | 0.98 | 0.272 | 661 | 293 |
| impact-stone | 0.99 | 0.444 | 626 | 105 |
| impact-rubber | 0.59 | 0.067 | 104 | 115 |
| impact-cardboard | 0.26 | 0.043 | 400 | 120 |
| pickup | 0.39 | 0.052 | 124 | 152 |
| **cover-mouth** | 0.41 | 0.159 | 562 | 146 |
| **health-station** | 0.72 | 0.159 | 744 | 183 |
| combine-tank | 0.98 | 0.297 | 454 | 81 |

`catch-light` vs `catch-heavy` is the headline: **2.4× apart in pitch and 1.9×
in duration.** If those two ever converge, the signature interaction of the game
has regressed to a single canned buzz, and `--analyze` will show it.

`rms` is in the table now because peak alone is misleading, and that cost this
project real time. Several effects reported as "too weak" on hardware peaked at
the same level as effects that worked; what separated them was level over TIME,
which is what skin integrates. `pickup` measured rms 0.019 before this revision
against `impact-wood`'s 0.133 — a seventh of the energy, from a peak column
that made them look merely a little different.

---

## Gravity gloves

The showcase interaction, and the part most changed in this revision.

| Feature | Level | Notes |
|---|---|---|
| Lock / pull / catch events arrive | **HARDWARE** | Confirmed previously. |
| Hand resolution via `hand_is_primary` | **STATIC** | Convention taken from the bHaptics integration. **Not independently confirmed at runtime** — if glove effects land on the wrong hand, suspect this first. |
| LOCK → held detent → PULL → CATCH as one sequence | **CODE** | The lock detent now *holds* until the game says the lock ended, instead of expiring after 95 ms while still locked on. |
| Rising pull sweep / falling catch | **RUNTIME** | 85→430 Hz against 470→210 Hz. Deliberately mirrored. |
| **Mass-dependent catch** | **CODE** | New. `GLOVE_CATCH_MASS` carries mass, material and spin. |
| Catch no longer double-reports | **STATIC** | A catch used to emit `GLOVE_CATCH` *and* `PHYS_PICKUP` for the same object. Fixed. |

**How the mass layer works, and its one unproven assumption.** No VScript API
exposes the gravity glove's target, so mass cannot be known when the catch
fires. Instead the snap plays immediately (latency is the whole point of a
catch) and the object is identified when it lands in the hand one or two ticks
later, arriving as `GLOVE_CATCH_MASS` — inside the snap's own envelope, so it
layers rather than lags.

The assumption: **the object lands within 0.35 s of the catch event.** That
window is a guess. If it is wrong, catches will feel weightless and a
`PHYS_PICKUP` will appear in the debug log where a `GLOVE_CATCH_MASS` should be.
That is the single most likely thing in this revision to need a number changed.

---

## Physics — the rule this revision was built around

> **If the player's hand would not physically feel it, do not vibrate for it.**

| Feature | Level | Notes |
|---|---|---|
| Pickup / throw telemetry | **STATIC** | Mass, hand velocity, angular velocity, material. |
| **Persistent grip load** | **CODE** | New. Carrying something heavy loads the trigger for as long as you carry it. |
| **Inertia wobble** | **RUNTIME** | New. Low-frequency PCM only while a heavy held object is actually being swung. Measured silent at rest. |
| **Door interaction** | **CODE** | New, and the **most heuristic thing in the build**. See below. |

### Doors — what was verified, and what was not

Alyx raises no game event for opening a door, so this is polling. What was
verified, by reading the shipped `server.dll` rather than guessing:

- The classnames exist: `prop_door_rotating`, `prop_door_rotating_physics`,
  `func_door_rotating`.
- Doors are genuinely hand-interacted — the binary carries
  `vr_door_handle_interact_start_distance`, `vr_door_handle_interact_hold_distance`,
  `vr_door_mass` and `vr_hand_use_door_distance`, which only exist because
  hands move doors.
- Every VScript call used is present: `FindAllByClassnameWithin`,
  `GetAngularVelocity`, `GetCenter`, `GetMass`.

What was **not** verified: that the thresholds are right, or that it does not
misfire. The discriminator requires the door to be turning, a hand to be near
it, and that hand to itself be moving — a door swinging shut on its own fails
the third test. But a door's origin is its hinge rather than its handle, so
"near it" is necessarily generous.

**If you ever feel a door you are not touching, set `doors=false`.** That
switch exists because this is the one feature most likely to be wrong.

Hacking minigames were investigated and **deliberately not implemented** —
`prop_hlvr_holo_hacking_point_drag`, `_rod_pull`, `_sphere_trace` and
`_point_match` all exist with matching `vr_hacking_*_interact_distance`
convars, so it is feasible. But the elements are holographic projections; there
is no physical resistance in the fiction, so what the hand would feel is a
judgment call rather than a fact, and this project does not invent those.
| **Held-object impact** | **CODE** | New. The case that *should* fire and previously had none. |
| Released-object impact | **REMOVED** | See below. |
| Angular velocity used, not discarded | **RUNTIME** | Spin drives FM roughness on impact, flutter on throw, grind on catch. |
| Material inferred from model/classname | **STATIC** | 8 classes. A heuristic, and labelled as one. |
| Material used on throw | **STATIC** | Was parsed and discarded — a glass bottle and a steel pipe left the hand identically. Fixed. |
| Impact confidence | **CODE** | Scales level 0.55–1.0. A doubtful reading lands as a light knock rather than being asserted at full strength. |
| Per-hand impact rate limit | **CODE** | `impact_cooldown_ms`, previously parsed and never used. |

### What was inverted, and why it mattered

The old build tracked thrown props for 0.8 s **after they left the hand** and
fired an impact when they decelerated. **Every impact haptic in the build came
from that path.** A bottle smashing three metres away buzzed the controller —
the exact thing the design rules forbid.

Meanwhile the case that genuinely deserves feedback — an object striking
something *while still held* — had no detection at all.

Both are now the other way round. This also resolves melee: **Half-Life: Alyx
has no swingable melee weapon**, so hitting something means bashing it with a
grabbed prop, which is precisely the held-impact path. There is no separate
melee system to build, and claiming one would be fiction.

### The held-impact heuristic, stated honestly

A held prop follows the hand, so it decelerates whenever the hand does. The
discriminator is the *difference*: in a real collision the object sheds speed
the hand did not.

```
excess  = (objectSpeedLost) - (handSpeedLost)
impulse = mass * excess          -- a heuristic, NOT a contact impulse
```

This ignores the collision normal and any energy that went into rotation. There
is no collision callback available to VScript, so this is an inference and is
reported as one — every impact carries a confidence value, and low confidence
plays softer rather than being suppressed or asserted.

**The thresholds (`HELD_MIN_SPEED = 110`, `HELD_MIN_EXCESS = 95`, in Source
units/s) are unvalidated guesses.** They have never been compared against real
in-game numbers. Expect to tune them; `debug=true` prints every impact with its
impulse, mass, spin and confidence for exactly that purpose.

---

## Weapons

| Weapon | Level | Notes |
|---|---|---|
| Pistol | **STATIC** | Two-stage multi-position trigger with a distinct break; 284 Hz body. |
| Shotgun | **STATIC** | Heavier throughout, breaks later, 94 Hz body sustained 542 ms — the heaviest thing in the set. |
| SMG | **STATIC** | Multi-position *vibration* so chatter builds with trigger depth, rather than one flat buzz. |
| Empty-chamber cue | **STATIC** | Inferred shell count. Only ever *adds* a cue to a shot that already happened; it can never suppress one. |
| Reload / cycle clicks | **STATIC** | Ten distinct manipulation events, restoring the weapon profile automatically. |
| Two-handed support brace | **STATIC** | Support hand feels the frame, not the action. |
| **MELEE profile** | **NOT REACHABLE** | Alyx has no crowbar weapon. Kept for mods; **do not claim it works.** |
| **GRENADE profile** | **NOT REACHABLE** | Grenades are physics props, not trigger weapons — they will not raise `player_shoot_weapon`. Reaches haptics through the *throw* path instead. |

Weapon identity comes from three signals (switch string, polled hand
attachment, weapon-specific manipulation events). It is **not** read from native
memory — no `server.dll` hooks are used, deliberately.

---

## Deliberately not implemented

Each of these was removed or refused for a stated reason, not overlooked.

| Removed | Why |
|---|---|
| **JUMP** | No hand touches anything during a jump. Generic locomotion vibration. |
| **TELEPORT_START / FINISH** | Same. `teleport_finish` is retained internally as a heartbeat for arming the physics sampler, but emits nothing. |
| **KILL** | Fired for *every* entity dying anywhere on the map, including distant NPCs. A kill is a HUD cue in haptic clothing — no hand feels it. |
| **Released-object impacts** | The hand is not connected to the object any more. |
| **Headset rumble** | Out of scope by instruction. |
| **Footsteps / ambient buzz** | Never implemented. |
| **Native `server.dll` / `vphysics2` hooks** | Four hard-coded byte signatures that break on every Alyx patch, in exchange for one number (exact clip size). The vphysics2 hook is a *pickup flag*, not a collision callback — it does not provide the collision API it is sometimes assumed to. |

`MANTLE` and `LADDER` were **kept**: gripping a ledge and closing a hand around
a rung are genuine hand-contact events.

---

## The waveform revision (2026-08-27)

The trigger layer was finished in the previous round. This one is entirely the
grip actuator, and it was driven by three findings from `--analyze` rather than
by taste.

### A measurement bug came first

`--analyze` was **not reproducible**. Per-instance variation (±11% length,
±5.5% pitch) drew from a single RNG stream running across the whole suite, so
inserting one test case shifted the draw for every case after it. Adding two
bracing tests moved `glove-pull` from 463 ms to 408 ms with nothing in its
profile touched.

That matters because the collision report compares ratios against a 1.5×
threshold, and an 11% swing on each of two effects is enough to move a pair
across that line for reasons unconnected to the design. The RNG is now re-seeded
per test case: the variation stays in the signal path, so what is measured is a
real instance of what ships, but it is always the **same** instance. Two
consecutive runs now produce byte-identical output. **RUNTIME.**

Everything below was measured after that fix. Before it, some of it was noise.

### The double tick that was never double — **STATIC**

`GLOVE_LOCK`'s second transient was written with ten values for an eleven-field
struct, so the 32 ms meant for `delayMs` landed in `fmFreq` (an FM rate with
zero depth, which does nothing) and the delay defaulted to zero. Both ticks
fired on the same sample and summed into one.

The comment above it credited "two ticks 36 ms apart" as the fix that rescued
the cue. That separation had never existed. It explains why the acquisition cue
kept reading as a faint buzz across several rounds of retuning: every round
adjusted pitch and level, and the thing that was supposed to be carrying the
character was not there to adjust. `--dump-profiles` now shows `delay:32.0`.

### Coverage was the real gap

Nineteen signatures had no self-test, so nothing measured them and nothing could
report them drifting. That is where the damage had accumulated:

| | before | what it was |
|---|---|---|
| six `RAPID_*` events | one waveform, pitch changed | the whole SMG reload |
| sixteen world events | one `Body()` voice each, 42–170 ms | the campaign moments |
| `TWO_HAND_START/END` | one `Body()` voice, 55 ms | bracing a weapon |

Most sat at 42–66 ms. The hardware round had already settled what that means —
peak is not what skin integrates, **level over time** is — and these were in
exactly the state the reload clicks had been in before they were rebuilt.
`COVER_MOUTH`, the Jeff chapter, was 44 ms of a single tone for the moment the
game most wants you to feel your own hand.

The suite is now **51 signatures across 9 families**, and this is the third time
this project has found the same failure in whichever layer had no test.

### Results — **RUNTIME**

| | before | after |
|---|---|---|
| signatures measured | 29 | 51 |
| perceptual collisions | 3 | **0** |
| signatures limiting below 0.75 | 4 | **0** (lowest 0.80) |
| quietest non-silent rms | 0.019 (`pickup`) | 0.036 (`door-light`) |
| `--analyze` reproducible | no | **yes, byte-identical** |

Families are grouped by **co-occurrence** — two effects need telling apart when
you meet them in the same moment. Reload mechanisms are therefore grouped by
weapon rather than in one pile: you rack a pistol slide seconds after seating
its magazine, but a pistol slide and a shotgun shell are never in the same
gesture. `cover-mouth`, `levitate` and `combine-tank` are deliberately
ungrouped: each is a one-off set-piece, they would collide with each other on
duration by construction, and a report that cries wolf stops being read.

### Hardware verdicts — **HARDWARE**

Bench-tested on real Sense controllers, 2026-08-27. Five signatures were put up
for judgement because each carried a risk `--analyze` structurally cannot check.
All five were reported good.

| Signature | What was at risk | Verdict |
|---|---|---|
| `catch-light` / `catch-heavy` | The mass layer is separated by **tremolo** — fast shimmer against slow wobble — and the collision report is blind to rhythm. If this axis did not land, the light end of the game's signature interaction was resting on nothing measurable. | **good** |
| `glove-lock` | First time in the project's history it has actually been *two* ticks. It is also pinned as the faintest cue in the game, so the failure mode was "correct but imperceptible". | **good** |
| `cover-mouth` | Rebuilt with no transient at all and deliberately sited at the edge of noticing. | **good** |
| `smg-open` / `smg-close` | The extremes of the new five-step reload grid, 461 ms against 82 ms. If these two read as one mechanism the grid had failed. | **good** |

The tremolo result is the one that matters most. It confirms that **rhythm is a
usable design axis on this hardware**, which the rest of this revision leans on
in several places the collision report cannot verify — cardboard's four-hit
crush, metal's slow pulse, the barnacle's coiling grip. Those were all reasoned
by analogy to the same principle and now have one direct measurement behind
them.

### What this revision still does NOT claim

The five above were felt **on the bench**, one at a time, with no game running.
That is not the same as being felt in play, where effects overlap, fire in
sequence, and compete with each other for the limiter. Nothing here has yet been
confirmed **in-game**.

The remaining 47 signatures have not been individually judged on hardware at
all. They are RUNTIME: measured, distinct, and not clipping.

---

## Known gaps and the next things to check

1. **Nothing in this revision has been felt on hardware.** Everything new is
   CODE or RUNTIME. Run `--test` and report which signatures land wrong.
2. **`GLOVE_CATCH_MASS` window (0.35 s)** — the likeliest number to be wrong.
   With `debug=true`, a catch should log `[Catch] … mass=… heft=…`. A
   `[Alyx] PHYS_PICKUP` there instead means the window is too short.
3. **Held-impact thresholds** — expect tuning against real logged values.
4. **`hand_is_primary`** — if glove effects land on the wrong hand, this
   convention is the first suspect.
5. **`impact-cardboard` measures peak 0.17** and **`pickup` 0.20**. These may be
   below the threshold of being noticed at all. Intentionally the quiet end of
   the dynamic range, but unconfirmed — worth a verdict on hardware.
6. **Network console** is implemented and preferred but has not been observed
   connecting. `-condebug` remains the proven route.

---

## What this build does not claim

- No collision API. Impacts are inferred from velocity differentials.
- No exact clip or chamber state. Ammunition counts are inferred and are used
  only to colour a shot, never to decide that one happened.
- No exact physical impulse. `mass × Δvelocity` is a defensible heuristic and is
  labelled as one everywhere it appears.
- No native memory reads, no injection, no patched Valve binaries.
