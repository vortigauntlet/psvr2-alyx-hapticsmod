# What was taken from other games' haptics, and what was not

Survey of DualSense and PSVR2 implementations, filtered through one question:

> Does this correspond to something a **Half-Life: Alyx player's hand** would
> physically feel?

Rule 7 of this project is that nothing gets added merely because another game
has it. Most of what follows was therefore rejected — that is the point of the
exercise, not a failure of it.

Sources: the [DualSense Games Steam curator](https://store.steampowered.com/curator/45261472-DualSense-Games),
an r/PSVR thread on the best PSVR2 haptics, and developer statements where they
exist.

---

## Adopted

### 1. Weight is a state, not an event — *Death Stranding* + *Synapse*

Two independent implementations converge on the same answer.

**Death Stranding** (Kojima Productions) maps cargo weight to trigger
resistance: little resistance with light cargo, significant force once Sam is
carrying a full stack.

**Synapse** (nDreams, PSVR2) makes the adaptive trigger *be* the telekinetic
grip — to the point that squeezing an explosive object too tightly detonates
it. This is the closest analogue in existence to Alyx's gravity gloves, and it
runs on the same Sense hardware.

**Why it applies here.** Alyx is full of carrying heavy things, and this build
had the defect both games solve: mass was reported once at pickup, as a 260 ms
overlay, and then forgotten. A 20 kg crate felt heavy for a quarter-second and
weightless for as long as you actually carried it.

This is the same defect class already fixed twice in this project — a momentary
cue standing in for a persistent state, exactly like the gravity glove's lock
detent expiring while still locked on.

**Implemented** as `PHYS_HOLD`: a 4 Hz heartbeat from the game script while a
hand holds anything over 2 kg, driving a persistent trigger load scaled by mass.

Sent as a heartbeat rather than a hold/release pair deliberately, so it **fails
safe**. If the script dies, the map changes, or a release is missed, the
heartbeat stops and the load expires. Nothing can strand the trigger loaded.

### 2. Trigger fatigue is real — *Horizon Forbidden West*

Guerrilla's game director Mathijs de Jonge, on their prototypes: they set the
adaptive trigger values "relatively high", and "after a few minutes we already
felt some fatigue with the triggers."

Corroborated by general design guidance to avoid long constant resistance, and
by players reporting finger ache after an hour.

**Why it applies here.** The grip load above is held for as long as the player
carries something — precisely the case that warning is about, and a VR session
runs long. It is therefore capped at **5 of a possible 8** and sits mid-travel.
Weight is carried by the load being *constant*, not by it being strong.

### 3. Weight wobble on swung objects — *r/PSVR*

> "I love it when games use haptic wobble to add a feeling of 'weight' to melee
> weapons."

**Why it applies here.** Held-object impact is already Alyx's melee. Inertia is
the missing half: a heavy object being swung loads and unloads the hand
rhythmically, and that is genuinely felt.

**Implemented** on the same `PHYS_HOLD` heartbeat, which carries hand speed and
spin — but **gated on actual motion**. Standing still holding a crate produces
no waveform at all. Measured: `hold-still` renders at **0.00 peak**,
`hold-swing` at 0.34 peak / 83 Hz. That gate is what keeps this from becoming
the ambient buzzing the design rules forbid.

### 4. Absence of tension as information — *Horizon Forbidden West*

Guerrilla "use the absence of adaptive tension to help communicate when you're
out of ammo."

**Already implemented** before this survey — an empty weapon drops the trigger
slack, "nothing left to break". Recorded here as independent validation rather
than as something taken.

---

## Rejected, with reasons

| Idea | Source | Why not |
|---|---|---|
| Bullet whizzing past your head | Pistol Whip | Headset channel, removed from this project by instruction — and a passing bullet is not a hand sensation. |
| Rumble on crash | Gran Turismo 7 | Headset. No Alyx equivalent. |
| Headache pulses | Madison VR | Headset. |
| Rain felt on your face | Resident Evil 8 | Headset, and not a hand. |
| **Liquid sloshing as you tilt a bottle** | Metro Awakening | **The best idea in the thread, and still a no.** Alyx has no liquid simulation and no contents state on any prop. Implementing it would mean inventing physics the game does not have, which rule 4 forbids. It would be a lie that happened to feel nice. |
| Mining-laser overheat ramp | No Man's Sky | Elegant — trigger resistance as a live readout of an accumulating resource. Alyx has no heat, charge or overheat mechanic on any weapon. Nothing to read out. |
| Water and paddle resistance | Kayak VR: Mirage | No continuous-medium interaction in Alyx. |
| Driving feedback | Gran Turismo 7, GTA V | No vehicles under player control. |
| Per-foot terrain footsteps | Death Stranding | Golden rule. Your hands are not your feet. |
| Music-synced haptics | Rez Infinite, Beat Saber, Thumper | Alyx is not rhythm-based. |
| Object-type-dependent telekinesis feedback | *Control* (as suggested by critics) | Worth noting this was a **reviewer's wish**, not a shipped Remedy feature — so it is not evidence of anything. The idea itself is already implemented here as `GLOVE_CATCH_MASS`. |

---

## Candidate, deliberately not implemented

### Weapon upgrade state should change the trigger — *Into the Radius*

> "The adaptive triggers changing from safe, auto, semi ... really stood out."

Alyx has no fire selector, but it does have **weapon upgrades that genuinely
change firing behaviour** — burst fire, the shotgun's grenade launcher, the
autoloader. A weapon whose behaviour changed should not keep the same trigger.

The game script already receives `SHOTGUN_GL`, `SHOTGUN_AUTOLOADER` and
`RAPID_UPGRADE`, and currently treats all three as one-shot blips when at least
the first two are plainly **states** carrying an integer.

**Not implemented, because the meaning of that integer has not been verified.**
Guessing that `state=1` means "grenade launcher armed" and building a trigger
profile on it would be exactly the kind of invented semantics this project
refuses. It needs one debug session with the values logged.

That is the single highest-value item left on the list, and it is cheap once
the data exists.
