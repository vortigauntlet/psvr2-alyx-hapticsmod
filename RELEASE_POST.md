# Posting materials — v1.0

Everything below is ready to copy. The zip and its checksum are in `dist/`.

---

## FIRST: Steam Workshop will not take this

Worth knowing before you plan the release.

Half-Life: Alyx's Workshop distributes **addons** — map and asset packages built
with the Alyx Workshop Tools and published as `.vpk`. It has no mechanism for
shipping an executable, and this mod is fundamentally an executable: the Workshop
half is three small Lua files, and on its own it does nothing at all, because the
thing that talks to the PSVR2Toolkit CAPI is the `.exe`.

So the realistic distribution is **GitHub Releases + a Reddit post linking to it**.
Reddit will not host a zip either, so it needs somewhere to live.

### But the Workshop is still worth doing later, for a real reason

The addon currently installs by two routes, and only route 1 works — appending
`script_reload_code psvr2_haptics.lua` to `game\hlvr\cfg\skill_manifest.cfg`.
That is the one place this mod touches a Valve file, and it is also the thing a
game update can silently undo.

Route 2 — the clean `hlvr_addons` layout — is already written on every install
and does nothing, because Alyx only mounts addons that are **enabled**, and a
hand-installed folder never appears in `default_enabled_addons_list`.

**Publishing the Lua half to the Workshop is what makes route 2 start working.**
A subscribed addon gets enabled and mounted, `gameinit.lua` runs, and the
`skill_manifest.cfg` edit becomes unnecessary. That removes the only Valve-file
modification in the project and the only thing a patch can quietly break.

Not done yet, and it needs verifying that a script-only addon mounts during the
base campaign rather than only when launching its own map. Worth a test before
you promise it to anyone.

---

## Reddit post

**Title:**

> I built proper per-event haptics for Half-Life: Alyx on PSVR2 Sense controllers — no injection, and you can measure what it does before installing it

**Body:**

Half-Life: Alyx has no PSVR2 support beyond generic rumble, so I wrote a haptics
layer for the Sense controllers using the PSVR2Toolkit CAPI.

**What it does**

- Per-event PCM haptics — gravity gloves, per-weapon reload mechanisms, held-object
  impacts with eight material classes, doors, carrying weight
- Adaptive trigger profiles per weapon, with recoil built as resistance-load →
  low-frequency kick (measured on hardware, not guessed)
- Campaign moments: covering your mouth in the Jeff chapter, barnacles, the health
  station, heaving Combine barriers, two-handed levitate, mantling, tripmine hacks
- Gravity glove catches are **mass-dependent** — a 0.4 kg bottle and a 22 kg crate
  land 2.4× apart in pitch and 1.9× apart in duration, and they differ in rhythm too

**One design rule, applied strictly**

> If your hand would not physically feel it, it does not vibrate.

So there is no footstep rumble, no jump or teleport buzz, no kill confirmation, and
no vibration for objects that have already left your hand. Those are all deliberate
absences, not gaps — a controller can only honestly represent what the hand touches.
Headset rumble is not used at all.

**No injection**

No DLL injection, no pattern scanning, no patched Valve binaries. The game side is
a VScript addon. It does add one line to one Valve *text* file
(`skill_manifest.cfg`); `Uninstall.bat` removes it. That is the whole footprint.

**You can check what it does before you trust it**

This is the part I actually care about. Run **Measure Haptics (no headset).bat** and
it renders every one of the 51 tactile signatures offline and prints what the
waveform really is — peak, RMS, duration, dominant frequency, and whether the
limiter is squashing it. No headset, no game, nothing installed.

It also reports **perceptual collisions**: any two effects in the same family that
sit inside the ~1.5× ratio skin needs before it can tell two vibrations apart. That
turns "this feels samey" into a number. Current build reports zero.

**Requirements**

- PSVR2 on PC with [PSVR2Toolkit](https://github.com/BnuuySolutions/PSVR2Toolkit) installed
- `-condebug` in Alyx's Steam launch options (this is what makes the game write the
  log the mod reads)
- Windows, x64

**Install:** unzip anywhere, run `Start Haptics.bat`. It finds Steam and Alyx itself.
**Uninstall:** `Uninstall.bat`.

**Honest status:** the tactile design in this build has been *measured*, not *felt* —
I rebuilt a lot of it this week and have not done a full hardware pass since. The
adaptive triggers have been verified on hardware. `docs/VERIFIED.md` in the download
grades every single claim at one of five levels (CODE / STATIC / RUNTIME / HARDWARE /
NOT IMPLEMENTED) and is explicit about what is inferred, what is a guess, and what is
known to be unreachable. If something feels wrong, `--analyze` output plus what you
felt is the most useful bug report you can send.

Source and download: https://github.com/vortigauntlet/psvr2-alyx-hapticsmod/releases/latest

SHA256 of the zip:
`2DA91004A26AD36A967B6CE8A64CC3C2B9D3C15039FE30FCD15CB496255395CD`

---

## Notes for the GitHub release description

Same as above, minus the Reddit framing. Attach:

- `dist/psvr2-alyx-haptics-1.0.zip`
- `dist/psvr2-alyx-haptics-1.0.sha256`

---

## Two things to expect on release day

**1. SmartScreen.** The exe is unsigned, so Windows will show "Windows protected your
PC" on first run. Say so in the post before someone else does — it reads far better
coming from you. The checksum is published for the same reason. Signing needs a code
signing certificate (~$100–400/yr), which is probably not worth it yet.

**2. "Where do I get PSVR2Toolkit?"** This will be the single most common question,
because the mod's error message when the DLL is missing does not currently link
anywhere. Consider pinning it in the post.
