#include "psvr2_sendprops.h"

#include "psvr2_emit.h"

#include "tier0/dbg.h"
#include "const.h"

#include <cstring>

namespace psvr2h {
namespace {

ServerClass* g_classes = nullptr;
bool g_resolved = false;
ResolvedProps g_props;

// Depth-first walk of one table, following nested data tables.
//
// `baseOffset` accumulates the offset of each enclosing table, which is what
// makes a property inside DT_LocalWeaponData resolve to an offset from the
// start of the entity rather than from the start of the sub-table.
bool SearchTable(SendTable* table, const char* propName, int baseOffset,
                 PropRef& out, int depth) {
    if (table == nullptr) return false;
    // Send tables are shallow in practice; the guard is against a malformed or
    // cyclic table taking the game down with a stack overflow rather than
    // against any structure Valve actually ships.
    if (depth > 8) return false;

    const int count = table->GetNumProps();
    for (int i = 0; i < count; ++i) {
        SendProp* prop = table->GetProp(i);
        if (prop == nullptr) continue;
        const char* name = prop->GetName();
        if (name == nullptr) continue;

        if (prop->GetType() == DPT_DataTable) {
            // Exclude-props and baseclass links have no useful offset of their
            // own; recursing through them is how a nested member is found.
            if (SearchTable(prop->GetDataTable(), propName,
                            baseOffset + prop->GetOffset(), out, depth + 1)) {
                return true;
            }
            continue;
        }

        if (strcmp(name, propName) == 0) {
            out.offset = baseOffset + prop->GetOffset();
            out.type = prop->GetType();
            return true;
        }
    }
    return false;
}

PropRef Resolve(const char* table, const char* prop, ResolvedProps& into) {
    ++into.total;
    PropRef ref = FindProp(table, prop);
    if (ref.valid()) {
        ++into.resolved;
    } else {
        // Named explicitly, because the whole point of resolving by name is
        // that a rename is REPORTED rather than silently read as garbage.
        Warning("[PSVR2H] could not resolve %s::%s - the events that depend on "
                "it will not be sent\n", table, prop);
    }
    return ref;
}

} // namespace

void ResolveProps(ServerClass* classes) {
    g_classes = classes;
    g_props = ResolvedProps{};

    g_props.playerActiveWeapon =
        Resolve("DT_BaseCombatCharacter", "m_hActiveWeapon", g_props);
    g_props.playerHealth  = Resolve("DT_BasePlayer", "m_iHealth", g_props);
    // Half-Life 2 specific, and the only one here that legitimately may be
    // absent on a heavily modified build. Its loss costs the armour half of the
    // damage reading and nothing else.
    g_props.playerArmor   = Resolve("DT_HL2_Player", "m_ArmorValue", g_props);
    g_props.weaponClip1   = Resolve("DT_BaseCombatWeapon", "m_iClip1", g_props);
    g_props.weaponClip2   = Resolve("DT_BaseCombatWeapon", "m_iClip2", g_props);
    g_props.weaponNextPrimary =
        Resolve("DT_BaseCombatWeapon", "m_flNextPrimaryAttack", g_props);
    g_props.weaponNextSecondary =
        Resolve("DT_BaseCombatWeapon", "m_flNextSecondaryAttack", g_props);

    g_resolved = true;
    Msg("[PSVR2H] resolved %d/%d network properties by name\n",
        g_props.resolved, g_props.total);
}

bool PropsResolved() { return g_resolved; }

const ResolvedProps& Props() { return g_props; }

PropRef FindProp(const char* tableName, const char* propName) {
    PropRef out;
    for (ServerClass* c = g_classes; c != nullptr; c = c->m_pNext) {
        SendTable* table = c->m_pTable;
        if (table == nullptr || table->GetName() == nullptr) continue;
        if (strcmp(table->GetName(), tableName) != 0) continue;
        if (SearchTable(table, propName, 0, out, 0)) return out;
    }

    // Not found under the exact table asked for. Fall back to searching every
    // class, because a mod is free to rename or re-parent a data table while
    // keeping the member itself - and a member found under a different table
    // is still the member we wanted.
    for (ServerClass* c = g_classes; c != nullptr; c = c->m_pNext) {
        if (SearchTable(c->m_pTable, propName, 0, out, 0)) return out;
    }
    return PropRef{};
}

int ReadInt(const void* entity, const PropRef& p, int fallback) {
    if (entity == nullptr || !p.valid()) return fallback;
    const char* base = static_cast<const char*>(entity);
    return *reinterpret_cast<const int*>(base + p.offset);
}

float ReadFloat(const void* entity, const PropRef& p, float fallback) {
    if (entity == nullptr || !p.valid()) return fallback;
    const char* base = static_cast<const char*>(entity);
    return *reinterpret_cast<const float*>(base + p.offset);
}

int ReadEHandleIndex(const void* entity, const PropRef& p) {
    if (entity == nullptr || !p.valid()) return -1;
    const char* base = static_cast<const char*>(entity);
    const unsigned int raw = *reinterpret_cast<const unsigned int*>(base + p.offset);
    if (raw == 0xFFFFFFFFu) return -1;
    // An EHANDLE packs an entity index in the low bits and a serial number
    // above it. Verified: src/public/const.h NUM_ENT_ENTRY_BITS / ENT_ENTRY_MASK.
    return static_cast<int>(raw & ENT_ENTRY_MASK);
}

} // namespace psvr2h
