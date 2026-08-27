// Source surface properties -> the core's tactile material classes.
//
//     Source surface prop  ->  generic Material  ->  haptic profile
//
// Half-Life 2 is in a much better position than Half-Life: Alyx here. Alyx does
// not expose surface properties to VScript, so its game side guesses the
// material from the model path ("...if the name contains 'crate' it is wood").
// Source 1 exposes the real thing, so this layer reads ground truth.
//
// TWO INPUTS, and the order matters.
//
//   1. The surface property NAME, e.g. "metal", "glassbottle", "cardboard".
//      Read with IPhysicsSurfaceProps::GetPropName(). This is the finer signal
//      and is preferred, because it distinguishes things the character code
//      cannot - rubber and cardboard have no character code of their own.
//
//   2. The material CHARACTER CODE, a single char from decals.h - CHAR_TEX_WOOD
//      'W', CHAR_TEX_METAL 'M' and so on. Read from
//      surfacedata_t::game.material. Only twenty values exist, so this is the
//      reliable fallback for a prop name nobody has classified.
//
// Verified against Source SDK 2013:
//   src/game/shared/decals.h            CHAR_TEX_* character codes
//   src/public/vphysics_interface.h     IPhysicsSurfaceProps
//   src/public/const.h                  BREAK_* flags
//
// The names in the table below come from Valve's shipped
// scripts/surfaceproperties*.txt, which are game content rather than SDK
// source. Every one of them is therefore matched as a PREFIX and always has the
// character code underneath it as a backstop, so an unrecognised or renamed
// prop degrades to the right coarse class instead of to silence.

#pragma once

#include "core/materials.h"

#include <string>

namespace psvr2 {

// Maps a Source surface property name to a tactile class.
// Returns Material::Unknown when the name is not recognised, so the caller can
// fall back to the character code.
Material MaterialFromSurfaceProp(const std::string& propName);

// Maps a CHAR_TEX_* material character code to a tactile class.
Material MaterialFromCharCode(char code);

// The combined lookup an adapter should use: prop name first, character code as
// the backstop. `code` may be 0 when the plugin could not read one.
Material MaterialFromSource(const std::string& propName, char code);

// Maps the `material` field of the break_breakable game event, which is a
// BREAK_* flag rather than either of the above.
//
// Verified: src/public/const.h
//   BREAK_GLASS 0x01, BREAK_METAL 0x02, BREAK_FLESH 0x04,
//   BREAK_WOOD 0x08, BREAK_CONCRETE 0x40
Material MaterialFromBreakFlags(int flags);

} // namespace psvr2
