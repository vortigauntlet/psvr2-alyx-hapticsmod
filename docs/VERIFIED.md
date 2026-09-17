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
| Headset rumble | **CODE** | Bound optionally (absent export does not fail the load) and off by default. Four patterns, reasoned from the frequency-only ABI, **never measured** - `--hmd-sweep` exists to fix that. |

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
units/s) are still unvalidated** — but they are now *measurable*, which they
were not before.

The obstacle was never effort, it was instrumentation. The script reported only
impacts that **passed**, so a threshold set too high produced silence and left
nothing in the log to explain it. Logging just the accepted hits can reveal
false alarms but never misses, and a miss is the more likely failure.

Game script 7.1 adds `PHYS_CANDIDATE`: every near-miss, from floors well below
the real thresholds, so a recording brackets the decision boundary instead of
only recording its far side. `--impacts` reads that back and reports the
distribution, what each candidate threshold would accept, and how much of each
deceleration the hand's own movement explains.

| Claim | Level |
|---|---|
| Candidates are logged and never produce a haptic | **STATIC** — the router returns before any handler; verified by reading the path |
| `--impacts` parses a recording and reports correctly | **RUNTIME** — exercised against a synthetic recording with a known answer; it found the planted plateau |
| The thresholds are correct | **NOT ESTABLISHED** — needs one session of deliberately bashing things |

What to look for is a **plateau**: a range where the accept count barely moves
means real hits and arm movement are cleanly separated. No plateau would mean
they overlap and no threshold can separate them — which is a finding about the
discriminator, not about the numbers.

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
| **Headset rumble for HAND events** | Recoil, impacts and gloves stay off the headset. Your skull does not recoil. The headset has its own four-event list; see the README. |
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

### Headset rumble — **CODE**, and honestly so

Added after a public request for it, as an opt-in. Everything about it is at
the CODE level and none of it has been felt, because verifying it needs a
jailbroken headset.

| Claim | Level |
|---|---|
| `psvr2_toolkit_set_hmd_rumble` exists in the shipped DLL | **STATIC** — read out of the PE export table; 12 exports, this is one |
| Its signature is `int(uint8_t rumbleHz)` | **STATIC** — confirmed against upstream `psvr2tk_capi.h`, not inferred |
| Binding it does not disturb the hand channel | **RUNTIME** — `--analyze` output is byte-identical to the build before this change |
| The channel enables, plays and exits cleanly | **RUNTIME** — `--hmd-test` runs to completion |
| The four patterns feel like anything | **CODE** — untested. Needs a jailbroken headset |
| The frequencies are the right frequencies | **CODE** — reasoned only. `--hmd-sweep` is the fix |

Two things worth stating plainly rather than burying:

**We cannot detect from software whether the headset will respond.** The probe
calls `set_hmd_rumble(0)`, which is the off value and therefore the one call
guaranteed safe to make blind. Its return code is recorded but deliberately not
used to gate availability, because this DLL is already known to return an
uninitialised register from `write_pcm` and 1-instead-of-0 from `wait_for_pcm`
— treating a non-zero here as failure would disable the feature for everyone
based on ABI noise. A headset that has not been jailbroken simply ignores the
value, which is exactly the safe fallback wanted.

**No headcrab event exists.** The request that prompted this was specifically
about facehuggers. Alyx raises nothing when one latches on; the damage arrives
as `player_hurt` like any other injury, so it reaches the headset through
`HURT` and cannot be told apart from being shot. Claiming facehugger support
would be inventing a semantic the game does not expose.

### What this revision still does NOT claim

The five above were felt **on the bench**, one at a time, with no game running.
That is not the same as being felt in play, where effects overlap, fire in
sequence, and compete with each other for the limiter. Nothing here has yet been
confirmed **in-game**.

The remaining 47 signatures have not been individually judged on hardware at
all. They are RUNTIME: measured, distinct, and not clipping.

---

## Glide width, and a response-curve limit found the hard way (2026-08-29)

### `ResponseGain()` over-promises above ~300 Hz — **HARDWARE VERIFIED**

The most useful thing to come out of this round, and it came from a rejected
build.

Most weapon bodies glided only 1.10-1.17x, under the ~1.5x skin needs to
resolve pitch — a parameter being paid for and not felt. The first fix widened
them at **constant mean frequency**, so `domHz` moved at most 1 Hz across both
games and the collision report stayed clear. It measured perfectly.

On hardware **every weapon read thinner** and the build was rejected.

Holding the mean while widening necessarily throws the ONSET up to 340-510 Hz.
`ResponseGain()` answers with digital gain — but a voice coil at 450 Hz
physically displaces less than at 250 Hz for the same drive, and digital gain
cannot buy back force the actuator is not producing. The response table is
therefore **not a valid equal-loudness compensation at the top of the band**;
it flattens the measured signal, not the felt one. Trimming amplitudes to
protect the limiter compounded it.

**Rule taken from this: never raise a voice's onset frequency to buy anything.**
Above roughly 300 Hz, amplitude on paper and force in the hand come apart.

### The version that shipped instead — **RUNTIME**

Hold `f0` exactly as it was and pull `f1` **down** into the 120-300 Hz strong
band, so each effect spends *more* of its life where the actuator is powerful.
No amplitude was trimmed anywhere — trimming is what "thin" means.

| effect | before | after | ratio | rms | limiter |
|---|---|---|---|---|---|
| Alyx pistol | 300→265 | 300→195 | 1.54x | 0.151 → 0.151 | 1.00 → 1.00 |
| Alyx SMG | 330→300 | 330→215 | 1.53x | 0.104 → 0.102 | 0.90 → 0.90 |
| Alyx hurt | 130→95 | 130→82 | 1.59x | 0.216 → **0.236** | 0.97 → 0.97 |
| Alyx melee | 135→100 | 135→85 | 1.59x | — | — |
| HL2 pistol | 300→265 | 300→195 | 1.54x | 0.121 → 0.121 | 1.00 → 1.00 |
| HL2 SMG | 245→210 | 245→160 | 1.53x | 0.090 → **0.091** | 1.00 → 1.00 |

**No limiter value moved anywhere in either game.** `domHz` drops (pistol
267 → 236) and that is accepted rather than engineered around: the collision
report is what protects the pitch ladder, and it still reads *none* for both
games.

Untouched: both shotguns, the grenade, the gravity pull, the magnum and the RPG
already glide wide. The crossbow and the shock are defined by the ABSENCE of
low end, so a downward sweep is precisely wrong for them. The RPG was tried at
a lower onset (110 Hz) and reverted — that lands in the 80-110 Hz dip, where
the compensation applies a 1.4x boost and drove its limiter from 0.99 to 0.83.

### Fine-tuning pass, and three things that did NOT pan out — **RUNTIME**

The downward-widening above was confirmed on hardware as a clear improvement.
Looking for more, most of the obvious moves measured as nothing:

1. **Widening the tails further does nothing.** Pistol rms is 0.151 whether the
   tail sits at 195 Hz or 125 Hz — flat across every value tried, for all three
   weapons. The force came from REMOVING the high onset, not from adding low
   tail, because the response curve is already flat across 125-300 Hz and an
   exponential decay front-loads the energy anyway. Going wider only lowers
   `domHz` and spends pitch separation for nothing.
2. **Lowering transients hurts where the limiter is healthy.** The pistol goes
   1.00 → 0.94 as its transient drops 470 → 330, because a lower accent starts
   summing constructively with the body onset instead of sitting clear of it.
3. **Materials are the wrong target entirely.** They are built on five
   deliberate pitch SLOTS (~55/130/200/310/470) with glass and plastic sharing
   one on purpose. Widening those collapses the separation that makes them
   distinguishable at all.

What did survive: dropping the crossbow and shock **transients** from 500 to
400 Hz. Those two were the most limited effects in the game, and doctrine says
what a limiter squashes first is the transient — so they were paying for their
brightness by losing their edge, while a 500 Hz accent cannot deliver what the
compensation promises anyway.

| effect | limiter | rms |
|---|---|---|
| crossbow | 0.83 → **0.87** | 0.110 → 0.108 |
| damage-shock | 0.86 → **0.90** | 0.080 → 0.079 |
| shock-hand | 0.82 → **0.85** | 0.087 → 0.086 |

Their bodies are deliberately NOT widened: past the transient change every
further step cost rms and bought no headroom. Everything in both effects still
sits above 380 Hz, so "the sharpest, brightest thing here" still is. Alyx is
untouched by this pass.

### Layer collision, not loudness — **RUNTIME**

The Alyx SMG was the QUIETEST weapon in the game (rms 0.102) and at the same
time the MOST limited (0.90). That combination is not a loud effect being tamed
— it is three layers all striking at t=0, summing past the ceiling, and the
limiter taking the difference out of the transient.

Staggering one supporting body layer 10 ms off the attack fixed both ends at
once:

| | before | after |
|---|---|---|
| peak | 0.98 | 0.95 |
| rms | 0.102 | **0.107** |
| limiter | 0.90 | **0.95** |

Nothing was made louder. Energy that was being squashed now survives. Duration
and `domHz` are unchanged. The plateau runs from 6 to 28 ms so this is not a
knife-edge; 10 ms was chosen because `GLOVE_LOCK` establishes that 32-36 ms
reads as two distinct ticks, and an SMG round must stay one event.

The same trick was tried on every other stacked effect and mostly did NOT
replicate: the explosion oscillated between 0.78 and 0.82 with no plateau, the
crossbow spiked at one delay only, and Alyx `hurt` got slightly worse. Those
are phase alignment, not a mechanism, and were left alone. Only `HL2_DAMAGE`
showed a consistent plateau (0.89 → 0.90, rms +2%) and took the change.

**Generalisable rule: when an effect is limited but not loud, suspect layer
collision before touching any amplitude.**

### The shotgun: what the collision report cannot see — **RUNTIME**

Reported from hardware as not feeling *unique*, while passing the collision
report comfortably: 3.0x the pistol's duration and 2.7x its pitch. The report
was not wrong, it was **blind**. It measures duration and pitch, and the
shotgun's problem was in neither.

It had **no modulation at all** and a transient of **0.34 — the same accent
amplitude as the pistol.** So the heaviest weapon in the game had the attack of
a handgun and, per the note on `Voice::amDepth`, the temporal signature of a
solid knock. It was a long low version of everything else.

Every attempt to simply enlarge the crack failed, because the transient and the
body both struck at t=0 and summed past the ceiling — raising the transient
alone took the limiter from 0.94 to **0.90** and bought nothing. The fix was
the layer-stagger rule found on the SMG: start the body **16 ms after** the
transient, which is also the physically honest order (crack, then mass).

| | before | after |
|---|---|---|
| Alyx shotgun | rms 0.352, lim 0.94 | rms 0.349, lim **0.98** |
| Alyx shotgun-empty | rms 0.357, lim 0.94 | rms 0.351, lim **0.95** |
| HL2 shotgun | rms 0.337, lim 0.94 | rms 0.321, lim **1.00** |
| HL2 shotgun-double | rms 0.385, lim 0.85 | rms 0.371, lim **0.89** |

With the attack window no longer shared, the body could go back **up** (0.76 →
0.84) — reversing a cut made when it had to fight the transient — and still
limit less than before. That reclaimed headroom is what pays for a 0.28-depth
6 Hz shudder, roughly 3.5 heaves across the effect: the weapon shaking itself
out. Nothing else in Alyx is both this long and modulated.

HL2 is held to 0.78 / 0.24 rather than 0.84 / 0.28 because its double-barrel
secondary rescales this profile to 0.82x frequency, dropping the body to ~37 Hz
where the compensation applies its largest boost. At the Alyx values the single
shot measured fine and the double regressed to 0.83.

**The transient increase (0.34 → 0.60) is not measurable and is not claimed to
be.** A 16 ms accent barely moves rms across a 574 ms effect. Its entire
benefit is the part rms cannot see, and only hardware can settle it.

### The pistol, and why the stagger had to come first — **RUNTIME**

Reported from hardware as feeling good, so this is the one change in the pass
made to something that was not complained about. The justification is that its
transient sat at **470 Hz** — inside the region the rejected build proved this
hardware cannot deliver. The accent was being paid for in headroom and arriving
as almost nothing, leaving the weapon carried by its body alone.

This was **not fixable before the layer-stagger existed**. Lowering the
transient toward the body's 300 Hz onset made the two sum coherently instead of
sitting clear, and the limiter went 1.00 → 0.94 for no gain — measured, and the
reason an earlier pass abandoned the idea. With the body moved 10 ms back the
collision disappears and the accent can be both lower and larger at once:

| | before | after |
|---|---|---|
| transient | 470 Hz @ 0.34 | **380 Hz @ 0.45** |
| peak | 0.92 | **0.87** |
| rms | 0.151 | 0.152 |
| limiter | 1.00 | 1.00 |

Delivered output of the accent rises roughly **47%** (0.45 x 0.78 response
against 0.34 x 0.70), while peak *falls*. No modulation was added: a pistol is
one clean snap, and the shudder that suits the shotgun would read here as a
mechanism rattling.

Duration moves 186 → 196 ms, which pulls the pistol/grenade duration ratio from
1.74x to 1.65x. Still clear of the 1.5x line, and it remains the thinnest
margin in the Alyx set — those two are 1.06x apart on pitch and separate on
duration alone.

**Three effects, one mechanism.** The SMG, both shotguns and both pistols were
all limited by layers striking together rather than by being loud. The rule is
now stated once: *when an effect is limited but not loud, suspect layer
collision before touching any amplitude.*

### The break stage the pistol never had — **RUNTIME**

Reported from hardware as wanting more punch. The cause was not a value being
too small; it was a whole stage being deleted before it reached the hardware.

Recoil is built as **break -> kick**: the trigger goes slack for a moment at
priority 7, and when that expires the kick underneath is revealed. A guard
exists to stop the break swallowing the kick — and it was a flat
`kMinRevealMs = 140`, against reveals of 35 ms (pistol) and 50 ms (shotgun).
Both failed it on **every shot** and had their break clamped to **zero**:

| weapon | rate | reveal | break before | break now |
|---|---|---|---|---|
| pistol | 28 Hz | 35 ms | **0 ms** | **35 ms** |
| shotgun | 12 Hz | 50 ms | **0 ms** | 30 ms |
| grenade | 20 Hz | 110 ms | 10 ms | 40 ms |
| smg | 22 Hz | 267 ms | 25 ms | 25 ms |

So the two weapons fired most often never performed the two-stage recoil this
file is built around, and nothing said so above debug level.

It hurt the pistol a second way. With the break dropped, the full 70 ms of kick
is exposed — about **two cycles** at 28 Hz — so it delivered a stutter where
the recoil bench calls for one completed pulse and states that *for a punch,
more cycles is the defect*. Restoring the break halves the exposed kick to
35 ms: 0.98 of a cycle, one crisp snap.

140 ms was inherited from the superseded "every effect needs >=1.5 cycles to be
felt" rule that this document already records as wrong for an impulse. The
floor is now **0.85 of one cycle of the kick's own rate**, which is what that
section concluded instead. The HL2 adapter had already been given a saner flat
55 ms; this generalises it rather than copying another magic number.

Note the `--analyze` recoil ladder was reporting the DESIGNED reveal all along,
not the delivered one, so it showed 35 ms for a pistol that was emitting 70 ms
with no break. The ladder is now truthful because the runtime matches it.

### Gravity-glove events went to the wrong hand — **STATIC**

Reported from hardware: catching with the LEFT glove rumbled the RIGHT hand.
Diagnosed without a further play session, by elimination.

Two mechanisms produce that exact symptom. The first — a wrong
`state.primaryIsLeft` — is **ruled out by the hardware reports themselves**: if
that flag were wrong, `pollWeapon()` would be inspecting the empty hand, every
weapon would resolve to `HANDS`, and all three guns would fall back to
`DEFAULT_FIRE` and feel identical. Several rounds of distinct per-weapon
feedback establish that weapon identity works, so the flag is right.

That leaves the field read, and there is a concrete bug in it.

```lua
if v ~= nil then markStyle("table"); return tonumber(v) or fallback end
```

`hand_is_primary` is a FLAG, and a flag arrives as a Lua boolean.
`tonumber(true)` is `nil`, so this returned the fallback `-1` for **both**
`true` and `false` — `sideOfPrimaryFlag()` then matched neither `0` nor `1` and
sent every lock, pull and catch to the primary hand, whichever glove was used.

`v ~= nil` is what made it silent rather than recoverable: in Lua
`false ~= nil` is **true**, so a false flag took the table branch and returned
the fallback instead of falling through to the accessor path that might have
answered.

| `hand_is_primary` | before | after |
|---|---|---|
| `false` (off hand) | right — **wrong** | left |
| `true` (primary) | right | right |
| `0` / `1` | correct | correct |
| absent | right | right — still warns once |

The numeric rows already worked, which is itself the proof of type: had the
engine delivered a number, this bug could not have occurred.

**One root cause, two manifestations.** `is_primary_left` on
`primary_hand_changed` is the same kind of flag read through the same helper,
with fallback `0` and compared `== 1` — so `state.primaryIsLeft` could never be
set to **true** even when the event did fire. The single fix repairs both. The
remaining `fieldNum` callers (`entindex`, `health`, `damagebits`, `state`) are
genuinely numeric and unaffected.

Separately, `primaryIsLeft` is only ever written by `primary_hand_changed` and
`single_controller_mode_changed`, which fire when the setting CHANGES and not
on map load — so a player who chose their hand once in the menu never raises
either. `pollWeapon()` now observes which hand actually holds a **firearm**
(never a tool, melee or prop, any of which can sit in the off hand) and
corrects the flag from that, accepting it only when exactly one hand qualifies.

**Confidence: high, not certain.** The one residual possibility is the field
being genuinely absent, which no amount of reading can distinguish from here —
that path still emits `WARN:hand_is_primary absent` once per session, so a log
settles it if the symptom survives.

### Still not claimed

Whether a 1.5x glide is perceptible *within* a single effect is still unproven.
The 1.5x figure is the threshold for telling two SEPARATE effects apart. This
round is defensible on force alone — nothing got quieter and two things got
louder — but if the glides still cannot be felt as movement, the honest
conclusion is that this hardware does not do intra-effect pitch travel and the
parameter should be spent elsewhere.

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

---

## Half-Life 2 VR integration (2026-08-27)

Graded on the same five levels as everything else. The short version: the
middleware half reaches **STATIC**, and the game side reaches **CODE** and no
further, because it has never been compiled.

### Reusable core - **STATIC**

The Alyx router was split into a game-agnostic core and a game adapter. The
proof that this changed nothing is a measurement rather than an assertion:
`--analyze` output before and after the refactor is **byte-identical** across
all 52 Alyx signatures, the collision report and the recoil ladder. The only
difference in the whole file is one added banner line naming the game.

Moved into `core/` unchanged: the material recipes, the impact synthesis, the
per-instance variation. Added: a game-independent event model (`core/events.h`),
an adapter interface (`core/adapter.h`), and a UDP transport.

### What the Half-Life 2 game side can observe - **STATIC**

Every `CreateEvent()` reachable from singleplayer in Source SDK 2013 was read.
The complete useful set is `player_hurt` (**no damage amount**), `entity_killed`,
`break_breakable` (**carries a material**), `break_prop`, `physgun_pickup`,
`weapon_equipped`, `ammo_pickup`, `take_health`, `take_armor`.

There is no `weapon_fire`, no reload event, no melee event, no explosion event.
`door_moving` exists but is `#ifdef CSTRIKE_DLL` and never fires in Half-Life 2 -
worth stating because assuming otherwise would have produced a door haptic that
silently never happened.

Everything else the plugin reports is **inferred** from polled state, and each
inference is documented at its site with what it watches and how it could be
wrong.

### The HL2VR source is not public - **STATIC**

Checked, because the whole integration strategy depends on it. The Source VR Mod
Team's FAQ says the mod can in principle be open sourced but has not been. The
only public HL2VR repository, `vittorioromeo/HL2VRU`, contains exactly two files
in its git tree - `.gitignore` and `README.md` - confirmed through the GitHub
API rather than by looking at the rendered page.

Consequence: direct source modification is not available, and a server plugin is
the cleanest remaining integration point rather than a fallback.

### Half-Life 2 tactile design - **STATIC**

62 signatures render and measure. The collision report finds **no pair** a hand
could not tell apart, across seven firing weapons, eight reload mechanisms, the
gravity gun family, melee, ten materials and the world set. Every weapon is
separable on the trigger ladder as well.

Two assertions are enforced by `--verify` rather than trusted:

* `melee-swing` renders **silent**. A crowbar swung through empty air must not
  vibrate, and this is the check that keeps it that way.
* `grav-hold-still` renders **silent**. Standing still holding a crate produces
  trigger load and no waveform at all.

Reaching zero collisions took three passes and the first one was wrong in an
instructive way: the eight reload mechanisms had each been built as the
mechanism it physically is, which made them different *code* and left them the
same *effect* - all eight inside 320-360 ms and 125-210 Hz, twelve flagged
pairs. Structure makes them feel like different mechanisms; spacing on the two
coarse axes is what makes them register as different events at all.

Two measurement bugs were found by the same report and are worth recording,
because both would have been invisible by feel:

* The mega-cannon reset in the test harness emitted its own 620 ms swell into
  the *next* case's measurement. `carry-grab` was measuring 670 ms of somebody
  else's waveform.
* Every gravity-gun release case primed itself with a grab, so each release was
  measuring the capture before it.

### Half-Life 2 game side - **CODE**

`src/games/hl2vr/plugin/` is written against the published Source SDK 2013
headers, with every interface, event and field cited against the file it was
read from. It has **not been compiled**, there is no SDK checkout or game
install on this machine, and the CMakeLists is a starting point rather than a
recipe that has been run.

Nothing about it should be described as working. The ordered list of what to
check first is in `docs/HL2VR.md` under "Runtime validation required"; the
largest single risk is whether HL2VR's VR grab still routes through
`Pickup_OnPhysGunPickup`, because the entire gravity-gun family depends on it.

### Explicitly not implemented, and why

* **Gravity gun charge-up.** Half-Life 2's gravity gun has no charge state.
  The brief listed one as an example; inventing it would be fabricated physics.
* **Which hand.** Half-Life 2 VR is a two-handed VR mod, but nothing in any
  network table a plugin can read says which hand holds the weapon. Everything
  goes to the configured primary hand. The `hand=` field is already in the wire
  format and the adapter already honours it, so this is one plugin line away if
  HL2VR ever exposes the state.
* **AR2 charged shot events.** The two-stage alternate fire is real and the
  adapter renders it, but the plugin does not yet emit the events - the timing
  signal has not been verified against a running game.

### The bHaptics transport - **STATIC**

Found by following up on "bHaptics made a mod for HL2VR", which was wrong in the
detail and right in the direction. There is no third-party mod: bHaptics' own
setup guide for the game is "install Half-Life 2, install the VR mod, press
play", because the Source VR Mod Team built the support in themselves.

Both bHaptics SDKs - the C# one, and the native C++ `haptic-library` whose
third-party dependency list names `easywsclient` - are WebSocket CLIENTS to
`ws://127.0.0.1:15881/v2/feedbacks`, sending
`{"Register":[{"Key":..}],"Submit":[{"type":"key","key":..}]}`. Register
announces the game's entire haptic vocabulary on connect.

So a game with built-in bHaptics support already broadcasts its own semantic
events to a local port. On a machine with no bHaptics software that port is
free. Implemented as `core/bhaptics_listener`, and it removes the need to build
anything at all.

Verified offline: the WebSocket handshake against RFC 6455's published test
vector, and the whole path - listener, JSON scan, key mapper, adapter,
synthesis - against a simulated client speaking the real protocol
(`tools/fake_hl2vr_bhaptics.py`).

NOT verified, and needs the game once: that Half-Life 2 VR uses this transport,
and its actual key names. `--bhaptics-scan` exists to answer both and asserts
nothing in the meantime.

### The key mapper refuses to guess - **STATIC**

An unmapped key produces silence and one line naming it. A wrong mapping would
fire the wrong sensation at the right moment, which is far harder to notice than
nothing happening - and that is not hypothetical: the first draft of the rule
table ordered "fire" before "damagefire", which turned burning damage into a
gunshot. `--verify` now pins seven ordering traps, each of which was a real bug:
DamageFire against PistolFire, CrowbarHit against the word "hit",
DamageExplosion against the word "damage", and so on.

### Damage by type - **STATIC**

Ported from bHaptics, who split damage by source where this project splits it by
what the arms feel. Five ways of being hurt, all in one collision family because
you meet them in the same firefight, and all separable: shock 100 ms/389 Hz,
bullet 208/125, explosion 343/84, fire 562/183, toxic 671/97.

Fire is the one worth noting: it has NO transient. Nothing struck you, and that
absence is most of what makes it read as burning rather than as being hit.

### Reading the manual overturned three assumptions - **STATIC**

The official Half-Life 2 VR manual documents the mod's VR interaction model, and
checking the adapter against it found three wrong claims. All three came from
the same mistake - reasoning about a VR mod from the game it is a mod OF, which
is the error this project warns about in other layers and then committed here.

1. **"Half-Life 2 has no two-handed weapon grip."** Asserted in a comment.
   HL2VR's Steam page advertises two-handed weapons as a headline feature and
   the manual says almost every weapon supports it; the shotgun REQUIRES it,
   because it cannot be pumped one-handed. Now modelled, including damped recoil
   when braced - the game genuinely reduces recoil two-handed, and haptics that
   contradict the game are worse than none.

2. **"Half-Life 2 refills a magazine in a single step."** True of the flat game.
   HL2VR's default is manual reloading with per-weapon physical steps: eject,
   catch the falling magazine, retrieve a fresh one from over the shoulder,
   insert it (differently per weapon), and chamber a round. The OFF hand does
   the inserting and chambering, so those are felt in both hands - the
   single-event version sent all of it to the primary hand, which was wrong
   about the most basic fact of the gesture.

3. **The shotgun pump was one waveform.** The manual: "pull in your off-hand
   towards your primary hand, then move it back... you will hear a sound for
   each part of the pump." Two separate player actions, arbitrarily far apart.
   A single two-stage waveform fires the second half before the player performs
   it.

The reload family regrouped as a consequence. It had been one flat "reloading"
family, which was correct while each weapon had one atomic reload and became
wrong once the sequence was modelled: the steps of a pistol reload happen
seconds apart and must not feel alike, while a pistol slide and a shotgun
forestock are never part of the same gesture.

78 signatures, no perceptual collisions.

### The harness caught a state leak - **STATIC**

`--verify` failed with "'brace' produced no waveform at all". Cause: the shotgun
test forces two-handed (the game requires it), `HL2_TWO_HAND` only emits on a
CHANGE, and the flag leaked between test cases - so by the time the brace case
ran, the state was already true.

Worth recording because it is the third time state leaking between test cases
has produced a silently wrong measurement, after weapon identity and the
mega-cannon swell. The fix is always the same: reset it in ResetForTest.

A second, subtler one in the same pass: the unbrace case measured the BRACE,
because it had to brace first to have anything to release. Both reported
154 ms / 195 Hz - the same waveform twice - and the collision report is what
made it visible.

### Three bugs found by actually running it - **STATIC**

The bHaptics listener and the plugin route were tested together against two
simulators speaking their real protocols. Doing that found three things that
static reading had not:

1. **The bHaptics socket never opened when the plugin was present.** The main
   loop only calls Connect() while the transport reports nothing connected, and
   the merged transport reports connected as soon as ONE source is up. So
   whichever route came up first permanently locked the other out. This would
   have shipped as "the bHaptics route just does not work if you built the
   plugin", and no amount of reading would have shown it.

2. **Redirecting output to a file produced an empty file.** iostreams fully
   buffer when stdout is not a console, so a session logged nothing until a
   clean exit - and nothing at all if the window was closed or the process
   killed. That is exactly the case a log is wanted for. Now line-flushed.

3. **The live path refused to start without the game installed.** Both routes
   are loopback sockets the game connects to, so they need it RUNNING, not
   installed - the one thing testable before installing anything was the one
   thing being refused. Now only install, uninstall and launch require the path.

Verified afterwards: `Event sources: plugin + bhaptics`, with a metal impact
carrying mass 40 and spin 700 from the plugin and the weapon vocabulary arriving
from the bHaptics stream, in the same session.
