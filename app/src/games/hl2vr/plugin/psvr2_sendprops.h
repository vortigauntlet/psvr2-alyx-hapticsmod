// Reading entity state without hardcoding a single offset.
//
// ---------------------------------------------------------------------------
// WHY THIS EXISTS
// ---------------------------------------------------------------------------
//
// Half-Life 2 fires almost no game events. There is no weapon_fire, so the only
// way to know a shot happened is to watch the weapon's magazine count fall -
// and that count is a member of a C++ class in a DLL whose source is not
// public.
//
// The usual answer in this space is a gamedata file full of byte offsets, which
// breaks on every game update. That is exactly the fragility this project
// refuses, so it is not what this does.
//
// Instead the offsets are looked up BY NAME at runtime, out of the server DLL's
// own network tables. Source publishes those tables through a documented public
// interface - IServerGameDLL::GetAllServerClasses() returns a linked list of
// ServerClass, each with a SendTable naming every replicated property and where
// it lives. Walking that list for "m_iClip1" asks the game where the field is
// rather than asserting it, so:
//
//   * an update that moves the field is followed automatically;
//   * an update that REMOVES the field fails loudly at load with the name that
//     went missing, instead of silently reading a neighbouring member;
//   * nothing has to be re-measured per game version, and there is no gamedata
//     file to maintain.
//
// Verified against Source SDK 2013:
//   src/public/eiface.h          IServerGameDLL::GetAllServerClasses
//   src/public/server_class.h    ServerClass, m_pNext, m_pTable
//   src/public/dt_send.h         SendTable, SendProp, GetName/GetOffset/
//                                GetDataTable/GetType
//
// ---------------------------------------------------------------------------
// WHAT IT CANNOT DO
// ---------------------------------------------------------------------------
//
// Only REPLICATED members are visible this way. A field the server never sends
// to a client - the gravity gun's m_bActive and m_hAttachedObject, for example -
// is not in any send table and cannot be read here at any price. Those are
// inferred from physics state instead, or simply not represented. See
// psvr2_plugin.cpp for which is which.

#pragma once

#include "dt_send.h"
#include "server_class.h"

namespace psvr2h {

// One resolved property: where it is, and how big.
struct PropRef {
    int offset = -1;
    SendPropType type = DPT_Int;
    bool valid() const { return offset >= 0; }
};

// Walks every server class once and caches what it found. Call after the
// interfaces are available (Load) and again on LevelInit, because a level
// change can bring new classes into the table.
//
// `missing` is filled with the names that could not be resolved, so the plugin
// can say precisely what it will not be able to report rather than quietly
// doing less.
void ResolveProps(ServerClass* classes);

// True once ResolveProps has run at least once.
bool PropsResolved();

// Looks up a property by "DT_ClassName" and member name, e.g.
// FindProp("DT_BaseCombatWeapon", "m_iClip1"). Recurses into nested tables, so
// m_iClip1 is found inside DT_LocalWeaponData without the caller having to know
// it lives there.
PropRef FindProp(const char* tableName, const char* propName);

// The properties this plugin needs, resolved once at load.
//
// Grouped in one struct so a diagnostic can print the whole set and a user can
// see at a glance which parts of the integration are live on their build.
struct ResolvedProps {
    PropRef playerActiveWeapon;  // CBaseCombatCharacter::m_hActiveWeapon
    PropRef playerHealth;        // CBasePlayer::m_iHealth
    PropRef playerArmor;         // CHL2_Player::m_ArmorValue
    PropRef weaponClip1;         // CBaseCombatWeapon::m_iClip1
    PropRef weaponClip2;         // CBaseCombatWeapon::m_iClip2
    PropRef weaponNextPrimary;   // CBaseCombatWeapon::m_flNextPrimaryAttack
    PropRef weaponNextSecondary; // CBaseCombatWeapon::m_flNextSecondaryAttack

    // How many of the above resolved. Anything less than the full set is
    // reported at load with the names that failed.
    int resolved = 0;
    int total = 0;
};

const ResolvedProps& Props();

// Typed reads. `entity` is a CBaseEntity* as an opaque pointer, because the
// plugin never needs the type - only the byte at a known offset.
int   ReadInt(const void* entity, const PropRef& p, int fallback = 0);
float ReadFloat(const void* entity, const PropRef& p, float fallback = 0.0f);
// Returns the entity index packed into an EHANDLE, or -1.
int   ReadEHandleIndex(const void* entity, const PropRef& p);

} // namespace psvr2h
