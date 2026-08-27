// PSVR2 Haptics - Half-Life 2 VR game-side plugin.
//
// ===========================================================================
// WHAT THIS IS
// ===========================================================================
//
// A Source server plugin (ISERVERPLUGINCALLBACKS003). It runs inside the game
// process, classifies gameplay into semantic events, and sends them to the
// PSVR2 haptics middleware. It touches no hardware and knows nothing about
// haptics; it is the Half-Life 2 counterpart of the Alyx VScript addon.
//
// It is loaded by the engine from hl2\addons\psvr2_haptics.vdf. No injection,
// no hooking, no patched game files, and nothing that a Half-Life 2 VR update
// can overwrite.
//
// ===========================================================================
// HOW MUCH OF THIS IS OBSERVED AND HOW MUCH IS INFERRED
// ===========================================================================
//
// This is the most important thing to understand about this file, and the
// reason it is commented as heavily as it is.
//
// Half-Life: Alyx broadcasts rich game events and its addon mostly listens.
// Half-Life 2 does not. Verified by reading every CreateEvent() call reachable
// from singleplayer in Source SDK 2013, the entire useful set is:
//
//   player_hurt        userid, health, attacker      (NO damage amount)
//   entity_killed      entindex_killed/attacker/inflictor, damagebits
//   break_breakable    userid, entindex, material    (player-broken only)
//   break_prop         userid, entindex
//   physgun_pickup     entindex                      (singleplayer only)
//   weapon_equipped    class, entindex, owner_entindex
//   ammo_pickup        ammo_index, amount, total
//   take_health        (baseentity.cpp)
//   take_armor         (hl2_player.cpp)
//
// There is no weapon_fire, no reload event, no melee event, no explosion
// event, and door_moving is #ifdef CSTRIKE_DLL so it never fires here.
//
// Everything else below is therefore DERIVED from polled state, and each
// derivation says what it is watching and how it could be wrong. Where a
// derivation is uncertain the event carries a confidence field, which the
// middleware uses to play a doubtful reading softer rather than either
// suppressing it or asserting it at full strength.
//
// ===========================================================================
// BUILDING THIS
// ===========================================================================
//
// It is NOT built by the middleware's CMake project and is not compiled by
// building this repository. It needs the Source SDK 2013 headers and libraries
// and a 32-bit toolchain. See README.md in this directory for the exact steps
// and for what has and has not been verified.

#include "psvr2_emit.h"
#include "psvr2_sendprops.h"

#include "engine/iserverplugin.h"
#include "eiface.h"
#include "igameevents.h"
#include "convar.h"
#include "edict.h"
#include "tier0/dbg.h"
#include "tier1/interface.h"
#include "toolframework/itoolentity.h"
#include "vphysics_interface.h"
#include "engine/IEngineTrace.h"
#include "cmodel.h"
#include "mathlib/vector.h"

#include <cmath>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// Engine interfaces.
//
// Every one is acquired by trying a list of version strings newest-first, so
// the plugin loads against both the branch Half-Life 2 VR was built from and a
// newer one. Failing to find an interface disables the features that need it
// and says so, rather than crashing on a null.
// ---------------------------------------------------------------------------
IVEngineServer*     g_engine = nullptr;
IServerGameDLL*     g_gamedll = nullptr;
IGameEventManager2* g_events = nullptr;
IServerTools*       g_tools = nullptr;
IPhysics*           g_physics = nullptr;
IPhysicsSurfaceProps* g_surfaces = nullptr;
IEngineTrace*       g_trace = nullptr;
ICvar*              g_cvar = nullptr;

template <typename T>
T* TryInterfaces(CreateInterfaceFn factory, const char* const* versions, int count) {
    if (factory == nullptr) return nullptr;
    for (int i = 0; i < count; ++i) {
        if (T* found = static_cast<T*>(factory(versions[i], nullptr))) {
            return found;
        }
    }
    return nullptr;
}

// How often the "still holding something" heartbeat goes out. The middleware
// refreshes a persistent trigger load from it and lets that load expire on its
// own, so a missed heartbeat costs a little weight rather than stranding the
// trigger loaded forever.
constexpr float kHoldIntervalSec = 0.25f;

// A held object's speed must fall by at least this much in one tick, from at
// least this speed, before the drop is treated as a collision rather than as
// the player simply slowing down.
//
// These are the Half-Life 2 equivalents of the two thresholds the Alyx addon
// carries, and like those they are STARTING POINTS rather than measured
// values - the units are engine units per second and nobody has yet swung a
// crate in this game with logging on. Both are exposed as cvars precisely so
// they can be measured rather than argued about, and every near-miss is
// reported as HL2_CANDIDATE so a recording contains the decision boundary and
// not just the far side of it.
ConVar psvr2_impact_minspeed("psvr2_impact_minspeed", "150", FCVAR_NONE,
                             "Held-object speed below which a deceleration is not an impact.");
ConVar psvr2_impact_minexcess("psvr2_impact_minexcess", "120", FCVAR_NONE,
                              "How much faster than the hand an object must stop to count as an impact.");
ConVar psvr2_console("psvr2_haptics_console", "1", FCVAR_NONE,
                     "Echo haptic events to the console as well as the socket.");
ConVar psvr2_debug("psvr2_haptics_debug", "0", FCVAR_NONE,
                   "Report near-miss impact candidates and unresolved state.");

// The engine's own convar for the supercharged gravity gun. Read rather than
// guessed: this is how the game itself decides.
ConVar* g_megaCannon = nullptr;

// ---------------------------------------------------------------------------
// Weapon state, polled.
// ---------------------------------------------------------------------------
struct WeaponState {
    int   entIndex = -1;
    char  classname[64]{};
    int   clip1 = -1;
    float nextPrimary = 0.0f;
    float nextSecondary = 0.0f;
    bool  valid = false;
};

struct PlayerState {
    int health = -1;
    int armor = -1;
};

// ---------------------------------------------------------------------------
// Held-object state, for the gravity gun and +USE carry.
// ---------------------------------------------------------------------------
struct HeldState {
    IPhysicsObject* object = nullptr;
    int   entIndex = -1;
    float mass = -1.0f;
    char  surfaceProp[64]{};
    char  charCode = 0;
    float lastSpeed = 0.0f;
    float lastSpin = 0.0f;
    Vector lastPos{0, 0, 0};
    float nextHoldEmit = 0.0f;
    float grabbedAt = 0.0f;
    bool  byCannon = false;
    void Clear() { *this = HeldState{}; }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Resolves a surface index to its property name and CHAR_TEX_* code.
//
// Both are sent, and the middleware prefers the name: the name distinguishes
// rubber and cardboard, which have no character code of their own, while the
// code is the reliable fallback for a prop nobody has classified.
void DescribeSurface(int surfaceIndex, char* nameOut, int nameLen, char& codeOut) {
    nameOut[0] = '\0';
    codeOut = 0;
    if (g_surfaces == nullptr || surfaceIndex < 0) return;
    if (const char* name = g_surfaces->GetPropName(surfaceIndex)) {
        V_strncpy(nameOut, name, nameLen);
        // Commas and newlines are the wire format's delimiters.
        for (char* p = nameOut; *p; ++p) {
            if (*p == ',' || *p == '\n' || *p == '\r') *p = ';';
        }
    }
    if (surfacedata_t* data = g_surfaces->GetSurfaceData(surfaceIndex)) {
        codeOut = data->game.material;
    }
}

float Now() { return g_engine != nullptr ? Plat_FloatTime() : 0.0f; }

float Length(const Vector& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

} // namespace

// ===========================================================================
// The plugin
// ===========================================================================
class CPsvr2HapticsPlugin : public IServerPluginCallbacks, public IGameEventListener2 {
public:
    // --- IServerPluginCallbacks ---
    bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory) override;
    void Unload() override;
    void Pause() override { paused_ = true; }
    void UnPause() override { paused_ = false; }
    const char* GetPluginDescription() override {
        return "PSVR2 Haptics for Half-Life 2 VR";
    }
    void LevelInit(char const* pMapName) override;
    void ServerActivate(edict_t*, int, int) override {}
    void GameFrame(bool simulating) override;
    void LevelShutdown() override;
    void ClientActive(edict_t*) override {}
    void ClientDisconnect(edict_t*) override {}
    void ClientPutInServer(edict_t*, char const*) override {}
    void SetCommandClient(int) override {}
    void ClientSettingsChanged(edict_t*) override {}
    PLUGIN_RESULT ClientConnect(bool*, edict_t*, const char*, const char*, char*, int) override {
        return PLUGIN_CONTINUE;
    }
    PLUGIN_RESULT ClientCommand(edict_t*, const CCommand&) override { return PLUGIN_CONTINUE; }
    PLUGIN_RESULT NetworkIDValidated(const char*, const char*) override { return PLUGIN_CONTINUE; }
    void OnQueryCvarValueFinished(QueryCvarCookie_t, edict_t*, EQueryCvarValueStatus,
                                  const char*, const char*) override {}
    void OnEdictAllocated(edict_t*) override {}
    void OnEdictFreed(const edict_t*) override {}

    // --- IGameEventListener2 ---
    void FireGameEvent(IGameEvent* event) override;

private:
    void PollWeapon(void* player);
    void PollPlayer(void* player);
    void PollHeld(void* player);
    void* LocalPlayerEntity();
    void* EntityFromIndex(int index);

    bool paused_ = false;
    WeaponState weapon_;
    PlayerState player_;
    HeldState held_;
    float nextTick_ = 0.0f;
    long long tickCounter_ = 0;
    bool megaWas_ = false;
    // Set by physgun_pickup and consumed on the next frame, when the object's
    // physics state can be read. The event fires before the object has been
    // attached, so reading mass and velocity inside the handler gets the state
    // from before the grab.
    int pendingPickup_ = -1;
    float pendingPickupAt_ = 0.0f;
    // A melee swing arms a window in which a re-traced surface is believed.
    float swingArmedUntil_ = 0.0f;
};

// ---------------------------------------------------------------------------

bool CPsvr2HapticsPlugin::Load(CreateInterfaceFn interfaceFactory,
                               CreateInterfaceFn gameServerFactory) {
    static const char* kEngine[] = {
        "VEngineServer023", "VEngineServer022", "VEngineServer021",
    };
    static const char* kGameDll[] = {
        "ServerGameDLL012", "ServerGameDLL011", "ServerGameDLL010",
        "ServerGameDLL009", "ServerGameDLL008",
    };
    static const char* kEvents[]  = {"GAMEEVENTSMANAGER002"};
    static const char* kTools[]   = {"VSERVERTOOLS003", "VSERVERTOOLS002", "VSERVERTOOLS001"};
    static const char* kPhysics[] = {"VPhysics031"};
    static const char* kSurf[]    = {"VPhysicsSurfaceProps001"};
    static const char* kTrace[]   = {"EngineTraceServer004", "EngineTraceServer003"};
    static const char* kCvar[]    = {"VEngineCvar007", "VEngineCvar004"};

    g_engine   = TryInterfaces<IVEngineServer>(interfaceFactory, kEngine, ARRAYSIZE(kEngine));
    g_events   = TryInterfaces<IGameEventManager2>(interfaceFactory, kEvents, ARRAYSIZE(kEvents));
    g_physics  = TryInterfaces<IPhysics>(interfaceFactory, kPhysics, ARRAYSIZE(kPhysics));
    g_surfaces = TryInterfaces<IPhysicsSurfaceProps>(interfaceFactory, kSurf, ARRAYSIZE(kSurf));
    g_trace    = TryInterfaces<IEngineTrace>(interfaceFactory, kTrace, ARRAYSIZE(kTrace));
    g_cvar     = TryInterfaces<ICvar>(interfaceFactory, kCvar, ARRAYSIZE(kCvar));

    g_gamedll  = TryInterfaces<IServerGameDLL>(gameServerFactory, kGameDll, ARRAYSIZE(kGameDll));
    g_tools    = TryInterfaces<IServerTools>(gameServerFactory, kTools, ARRAYSIZE(kTools));

    // The engine and the game DLL are the two that nothing works without.
    if (g_engine == nullptr || g_gamedll == nullptr) {
        Warning("[PSVR2H] could not acquire the engine or game interfaces; "
                "the plugin will not load.\n");
        return false;
    }
    if (g_events == nullptr) {
        Warning("[PSVR2H] no game event manager - damage, breakables and "
                "gravity gun pickups will not be reported.\n");
    }
    if (g_physics == nullptr || g_surfaces == nullptr) {
        Warning("[PSVR2H] no vphysics interface - held-object mass, material "
                "and impacts will not be reported.\n");
    }

    ConVar_Register();
    if (g_cvar != nullptr) {
        g_megaCannon = g_cvar->FindVar("physcannon_mega_enabled");
    }

    EmitInit();
    EmitSetConsole(psvr2_console.GetBool());
    Msg("[PSVR2H] PSVR2 Haptics plugin loaded (%s)\n",
        EmitHasSocket() ? "udp 127.0.0.1:29001 + console"
                        : "console only - the socket could not be opened");

    ResolveProps(g_gamedll->GetAllServerClasses());

    if (g_events != nullptr) {
        // Every singleplayer-reachable event this integration can use. Verified
        // by reading the SDK; anything not on this list is not fired by
        // Half-Life 2 and is inferred instead, or not represented at all.
        static const char* kListen[] = {
            "player_hurt", "entity_killed", "break_breakable", "break_prop",
            "physgun_pickup", "weapon_equipped", "ammo_pickup",
            "take_health", "take_armor",
        };
        for (const char* name : kListen) {
            if (!g_events->AddListener(this, name, true)) {
                Warning("[PSVR2H] this build does not define the '%s' event\n", name);
            }
        }
    }

    Emit("GAME", "hl2vr");
    Emit("SCRIPT_LOADED", "plugin-1.0");
    return true;
}

void CPsvr2HapticsPlugin::Unload() {
    if (g_events != nullptr) g_events->RemoveListener(this);
    ConVar_Unregister();
    EmitShutdown();
}

void CPsvr2HapticsPlugin::LevelInit(char const* pMapName) {
    weapon_ = WeaponState{};
    player_ = PlayerState{};
    held_.Clear();
    pendingPickup_ = -1;
    swingArmedUntil_ = 0.0f;
    // Send tables are rebuilt per level in some configurations, so the offsets
    // are re-resolved rather than assumed to survive.
    if (g_gamedll != nullptr) ResolveProps(g_gamedll->GetAllServerClasses());
    EmitF("MAP", "%s", pMapName != nullptr ? pMapName : "?");
    Emit("READY", nullptr);
}

void CPsvr2HapticsPlugin::LevelShutdown() {
    held_.Clear();
    // A level change with something in the beam would otherwise leave the
    // middleware's trigger load alive until its backstop expired.
    Emit("HL2_GRAV_DROP", "0");
}

void* CPsvr2HapticsPlugin::EntityFromIndex(int index) {
    if (g_tools == nullptr || index < 0) return nullptr;
    return g_tools->GetBaseEntityByEntIndex(index);
}

void* CPsvr2HapticsPlugin::LocalPlayerEntity() {
    // Singleplayer: entity 1 is the player. Read through IServerTools so the
    // result is the CBaseEntity the property offsets apply to.
    return EntityFromIndex(1);
}

// ---------------------------------------------------------------------------
// Per-frame polling
// ---------------------------------------------------------------------------
void CPsvr2HapticsPlugin::GameFrame(bool simulating) {
    if (paused_ || !simulating || !PropsResolved()) return;

    EmitSetConsole(psvr2_console.GetBool());

    void* player = LocalPlayerEntity();
    if (player == nullptr) return;

    PollWeapon(player);
    PollPlayer(player);
    PollHeld(player);

    // The supercharged gravity gun is a genuine state change and the game
    // decides it with this convar, so it is read rather than inferred.
    if (g_megaCannon != nullptr) {
        const bool mega = g_megaCannon->GetBool();
        if (mega != megaWas_) {
            megaWas_ = mega;
            EmitF("HL2_MEGA", "%d", mega ? 1 : 0);
        }
    }

    // Delivery heartbeat, so the middleware can measure what the transport is
    // doing to event timing. It carries the game's own clock, so bunched
    // arrivals and dropped datagrams are both visible.
    const float now = Now();
    if (now >= nextTick_) {
        nextTick_ = now + 0.25f;
        EmitF("TICK", "%lld,%.3f", ++tickCounter_, static_cast<double>(now));
    }
}

void CPsvr2HapticsPlugin::PollWeapon(void* player) {
    const ResolvedProps& p = Props();
    const int weaponIndex = ReadEHandleIndex(player, p.playerActiveWeapon);
    void* weapon = EntityFromIndex(weaponIndex);

    if (weapon == nullptr) {
        if (weapon_.valid) {
            weapon_ = WeaponState{};
            Emit("HL2_WEAPON", "");
        }
        return;
    }

    // The classname comes from IServerTools::GetKeyValue, which resolves it by
    // name through the entity's own datamap - no offset involved.
    char classname[64]{};
    if (g_tools != nullptr) {
        g_tools->GetKeyValue(static_cast<CBaseEntity*>(weapon), "classname",
                             classname, sizeof(classname));
    }

    const bool changed = (weaponIndex != weapon_.entIndex) ||
                         (strcmp(classname, weapon_.classname) != 0);
    if (changed) {
        weapon_ = WeaponState{};
        weapon_.entIndex = weaponIndex;
        V_strncpy(weapon_.classname, classname, sizeof(weapon_.classname));
        weapon_.clip1 = ReadInt(weapon, p.weaponClip1, -1);
        weapon_.nextPrimary = ReadFloat(weapon, p.weaponNextPrimary);
        weapon_.nextSecondary = ReadFloat(weapon, p.weaponNextSecondary);
        weapon_.valid = true;
        EmitF("HL2_WEAPON", "%s", classname);
        return;
    }

    const int clip = ReadInt(weapon, p.weaponClip1, -1);
    const float nextPrimary = ReadFloat(weapon, p.weaponNextPrimary);
    const float nextSecondary = ReadFloat(weapon, p.weaponNextSecondary);

    // ---- a shot -----------------------------------------------------------
    //
    // INFERRED, from the magazine count falling. Half-Life 2 fires no
    // weapon_fire event in singleplayer, so this is the only signal there is.
    //
    // It is reliable for every weapon that consumes ammunition and it cannot
    // false-positive on a reload, because a reload only ever raises the count.
    // What it cannot see is a weapon that fires without consuming a magazine
    // round - which in Half-Life 2 means the gravity gun and the crowbar, both
    // of which are handled separately below.
    if (clip >= 0 && weapon_.clip1 >= 0 && clip < weapon_.clip1) {
        const bool secondary = nextSecondary > weapon_.nextSecondary + 0.01f &&
                               nextPrimary <= weapon_.nextPrimary + 0.01f;
        EmitF("HL2_FIRE", "%s,%d,%d", "", clip, secondary ? 2 : 1);
    }
    // ---- a reload ---------------------------------------------------------
    //
    // The count rising. The shotgun raises it one shell at a time, which is a
    // real per-shell observation; every other weapon jumps straight to full,
    // and the middleware renders that as the multi-stage mechanism the weapon
    // physically has rather than pretending to have seen the stages.
    else if (clip > weapon_.clip1 && weapon_.clip1 >= 0) {
        if (clip == weapon_.clip1 + 1 && strstr(weapon_.classname, "shotgun") != nullptr) {
            EmitF("HL2_SHELL", "%d", clip);
        } else {
            EmitF("HL2_RELOAD", "%s", "");
        }
    }
    // ---- a swing, or a dry trigger ---------------------------------------
    //
    // The refire timer advancing with no ammunition consumed. For the crowbar
    // that is a swing; for a firearm with an empty magazine it is a dry
    // trigger. Both are real, and both are distinguishable from a shot because
    // a shot always moves the count.
    else if (nextPrimary > weapon_.nextPrimary + 0.01f && clip == weapon_.clip1) {
        const bool melee = strstr(weapon_.classname, "crowbar") != nullptr ||
                           strstr(weapon_.classname, "stunstick") != nullptr;
        const bool gravgun = strstr(weapon_.classname, "physcannon") != nullptr;
        if (melee) {
            swingArmedUntil_ = Now() + 0.35f;
            Emit("HL2_MELEE_SWING", "CROWBAR");
            // The surface the swing would have struck, traced the same way the
            // weapon traces it. Reported only when something is actually in
            // range: a swing through empty air must produce nothing, which is
            // the whole reason swing and hit are two events.
            //
            // APPROXIMATE, and marked as such. This trace runs from the
            // server's idea of the player's eye and forward vector; in VR the
            // crowbar is swung by a HAND, and this plugin cannot see where that
            // hand is. It will agree with the game most of the time and
            // disagree when the player swings well off their view direction.
            // See README.md - this is the single largest item needing runtime
            // validation.
            if (g_trace != nullptr && g_engine != nullptr) {
                edict_t* pl = g_engine->PEntityOfEntIndex(1);
                if (pl != nullptr) {
                    Vector eye = pl->GetCollideable() != nullptr
                                     ? pl->GetCollideable()->GetCollisionOrigin()
                                     : Vector(0, 0, 0);
                    eye.z += 64.0f; // approximate standing eye height
                    QAngle ang = pl->GetCollideable() != nullptr
                                     ? pl->GetCollideable()->GetCollisionAngles()
                                     : QAngle(0, 0, 0);
                    Vector fwd;
                    AngleVectors(ang, &fwd);
                    // 75 units: CBaseHLBludgeonWeapon::GetRange for the crowbar.
                    const Vector end = eye + fwd * 75.0f;
                    Ray_t ray;
                    ray.Init(eye, end);
                    trace_t tr;
                    g_trace->TraceRay(ray, MASK_SHOT_HULL, nullptr, &tr);
                    if (tr.fraction < 1.0f) {
                        char surf[64]; char code = 0;
                        DescribeSurface(tr.surface.surfaceProps, surf, sizeof(surf), code);
                        EmitF("HL2_MELEE_HIT", "CROWBAR,%s,%c,%.2f", surf,
                              code ? code : '?', 1.0f - tr.fraction * 0.35f);
                    }
                }
            }
        } else if (gravgun) {
            // Primary fire with nothing in the beam is a punt; with something
            // in it, it is a launch. Which one it was is decided by whether we
            // were holding anything a moment ago.
            if (held_.object != nullptr) {
                EmitF("HL2_GRAV_LAUNCH", "%.2f,%.0f", held_.mass, held_.lastSpeed);
                held_.Clear();
            } else {
                char surf[64] = "";
                char code = 0;
                EmitF("HL2_GRAV_PUNT", "%s,%c", surf, code ? code : '?');
            }
        } else if (clip == 0) {
            EmitF("HL2_DRYFIRE", "%s", "");
        }
    }

    weapon_.clip1 = clip;
    weapon_.nextPrimary = nextPrimary;
    weapon_.nextSecondary = nextSecondary;
}

void CPsvr2HapticsPlugin::PollPlayer(void* player) {
    const ResolvedProps& p = Props();
    const int health = ReadInt(player, p.playerHealth, -1);
    const int armor = ReadInt(player, p.playerArmor, -1);

    // player_hurt carries no damage amount in Half-Life 2, so the amount is the
    // health delta plus the armour delta. That is the honest reading and it is
    // what the event handler below uses; this poll is what makes it available.
    if (player_.health >= 0 && health >= 0 && health < player_.health) {
        const int lost = (player_.health - health) +
                         (armor >= 0 && player_.armor >= 0 && armor < player_.armor
                              ? (player_.armor - armor)
                              : 0);
        EmitF("HL2_DAMAGE", "%d,%d", lost, armor < 0 ? 0 : armor);
    }
    player_.health = health;
    player_.armor = armor;
}

void CPsvr2HapticsPlugin::PollHeld(void* player) {
    (void)player;
    if (g_physics == nullptr) return;

    const float now = Now();

    // Resolve a pending physgun_pickup one frame after the event, when the
    // object has actually been attached and its physics state is meaningful.
    if (pendingPickup_ >= 0 && now > pendingPickupAt_) {
        void* ent = EntityFromIndex(pendingPickup_);
        pendingPickup_ = -1;
        if (ent != nullptr) {
            // The physics object is found by walking the active object list and
            // matching GetGameData() against the entity, which is a documented
            // public path and needs no virtual call on CBaseEntity.
            IPhysicsEnvironment* env = g_physics->GetActiveEnvironmentByIndex(0);
            if (env != nullptr) {
                int count = 0;
                const IPhysicsObject** list = env->GetObjectList(&count);
                for (int i = 0; i < count; ++i) {
                    IPhysicsObject* obj = const_cast<IPhysicsObject*>(list[i]);
                    if (obj == nullptr || obj->GetGameData() != ent) continue;
                    held_.Clear();
                    held_.object = obj;
                    held_.entIndex = pendingPickup_;
                    held_.mass = obj->GetMass();
                    held_.grabbedAt = now;
                    DescribeSurface(obj->GetMaterialIndex(), held_.surfaceProp,
                                    sizeof(held_.surfaceProp), held_.charCode);
                    obj->GetPosition(&held_.lastPos, nullptr);
                    held_.byCannon =
                        strstr(weapon_.classname, "physcannon") != nullptr;
                    EmitF(held_.byCannon ? "HL2_GRAV_GRAB" : "HL2_CARRY_GRAB",
                          "%.2f,%s,%c", held_.mass, held_.surfaceProp,
                          held_.charCode ? held_.charCode : '?');
                    break;
                }
            }
        }
    }

    if (held_.object == nullptr) return;

    Vector vel(0, 0, 0);
    AngularImpulse ang(0, 0, 0);
    held_.object->GetVelocity(&vel, &ang);
    const float speed = Length(vel);
    const float spin = Length(Vector(ang.x, ang.y, ang.z));

    // ---- a held-object impact --------------------------------------------
    //
    // INFERRED, exactly as in the Alyx addon and for the same reason: Source
    // reports collisions to the game's own physics callback, not to a plugin.
    // What is visible is that the object was moving and then abruptly was not.
    //
    // The two thresholds are cvars and every near-miss is reported, so this is
    // measurable rather than argued about - see the note on the cvars above.
    const float drop = held_.lastSpeed - speed;
    const float minSpeed = psvr2_impact_minspeed.GetFloat();
    const float minExcess = psvr2_impact_minexcess.GetFloat();
    if (held_.lastSpeed > minSpeed * 0.5f && drop > minExcess * 0.5f) {
        const bool passed = held_.lastSpeed > minSpeed && drop > minExcess;
        if (passed) {
            // Confidence falls as the reading approaches the threshold, so a
            // marginal hit lands as a light knock rather than being asserted at
            // full strength or thrown away.
            const float margin = (drop - minExcess) / (minExcess * 2.0f);
            const float confidence = margin > 1.0f ? 1.0f : (0.55f + 0.45f * margin);
            const float impulse = drop * (held_.mass > 0.0f ? held_.mass : 1.0f);
            EmitF("HL2_IMPACT", "%.1f,%.2f,%s,%c,%.1f,%.2f", impulse, held_.mass,
                  held_.surfaceProp, held_.charCode ? held_.charCode : '?', spin,
                  confidence);
        }
        if (psvr2_debug.GetBool()) {
            EmitF("HL2_CANDIDATE", "%.1f,%.2f,%.1f,%.1f,%d,%s",
                  drop * (held_.mass > 0.0f ? held_.mass : 1.0f), held_.mass,
                  held_.lastSpeed, drop, passed ? 1 : 0, held_.surfaceProp);
        }
    }

    // ---- the carry heartbeat ----------------------------------------------
    if (now >= held_.nextHoldEmit) {
        held_.nextHoldEmit = now + kHoldIntervalSec;
        EmitF("HL2_GRAV_HOLD", "%.2f,%.0f,%.0f", held_.mass, speed, spin);
    }

    held_.lastSpeed = speed;
    held_.lastSpin = spin;
    held_.object->GetPosition(&held_.lastPos, nullptr);
}

// ---------------------------------------------------------------------------
// Game events
// ---------------------------------------------------------------------------
void CPsvr2HapticsPlugin::FireGameEvent(IGameEvent* event) {
    if (paused_ || event == nullptr) return;
    const char* name = event->GetName();
    if (name == nullptr) return;

    if (strcmp(name, "physgun_pickup") == 0) {
        // Carries only entindex, and fires for the gravity gun AND for a +USE
        // carry - Pickup_OnPhysGunPickup is called from both. Which one it was
        // is decided in PollHeld from the weapon currently held, one frame
        // later when the object's physics state is real.
        pendingPickup_ = event->GetInt("entindex", -1);
        pendingPickupAt_ = Now();
        return;
    }

    if (strcmp(name, "break_breakable") == 0) {
        // The only event in Half-Life 2 that hands us a MATERIAL, and it only
        // fires for something the player broke - both of which make it worth
        // listening to. The value is a BREAK_* flag, not a surface property.
        EmitF("HL2_BREAK", "%d", event->GetInt("material", 0));
        return;
    }

    if (strcmp(name, "break_prop") == 0) {
        // No material, and fires for props broken by anyone. Only reported when
        // the player did it: something collapsing across the room is not a hand
        // sensation, which is the rule this whole project is built on.
        if (event->GetInt("userid", 0) != 0) EmitF("HL2_BREAK", "%d", 0);
        return;
    }

    if (strcmp(name, "weapon_equipped") == 0) {
        // Fires on ACQUIRING a weapon, not on switching to one, so it is a
        // pickup cue rather than the weapon-change signal. The change itself is
        // polled, which catches every switch including those this never sees.
        EmitF("HL2_ITEM", "%s", event->GetString("class", "weapon"));
        return;
    }

    if (strcmp(name, "ammo_pickup") == 0) {
        EmitF("HL2_ITEM", "ammo_%d", event->GetInt("ammo_index", 0));
        return;
    }

    if (strcmp(name, "take_health") == 0) {
        Emit("HL2_HEALTHKIT", nullptr);
        return;
    }

    if (strcmp(name, "take_armor") == 0) {
        Emit("HL2_ITEM", "armor");
        return;
    }

    // player_hurt and entity_killed are deliberately NOT turned into haptics
    // here.
    //
    // player_hurt carries no damage amount in Half-Life 2, so the amount comes
    // from the polled health delta instead and emitting on both would double
    // every hit. entity_killed fires for every entity that dies anywhere, and
    // a kill is not something a hand physically feels - it was a HUD cue in
    // haptic clothing, and it was removed from the Alyx integration for the
    // same reason.
}

// ===========================================================================
// Export
// ===========================================================================
namespace {
CPsvr2HapticsPlugin g_plugin;
}

EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CPsvr2HapticsPlugin, IServerPluginCallbacks,
                                  INTERFACEVERSION_ISERVERPLUGINCALLBACKS,
                                  g_plugin);
