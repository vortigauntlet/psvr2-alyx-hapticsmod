#include "games/hl2vr/hl2vr_surfaces.h"

#include <algorithm>
#include <cctype>

namespace psvr2 {
namespace {

struct PropRow { const char* prefix; Material material; };

// Longest-prefix-wins, so "wood_crate" is matched before "wood" would swallow
// it and "glassbottle" before "glass". The table is ordered by descending
// prefix length at lookup time rather than by hand, because a hand-ordered
// table is one insertion away from being silently wrong.
const PropRow kProps[] = {
    // --- metal ---------------------------------------------------------
    {"metal", Material::Metal},
    {"metalgrate", Material::Metal},
    {"metalvent", Material::Metal},
    {"metalpanel", Material::Metal},
    {"metalvehicle", Material::Metal},
    {"chainlink", Material::Metal},
    {"chain", Material::Metal},
    {"grate", Material::Metal},
    {"canister", Material::Metal},
    {"metal_box", Material::Metal},
    {"metal_barrel", Material::Metal},
    {"metal_bouncy", Material::Metal},
    {"solidmetal", Material::Metal},
    {"strider", Material::Metal},
    {"combine_metal", Material::Metal},
    // A drink can is metal, but it is thin and it crumples. Thin sheet metal
    // rings far less than a barrel does, and the class that actually reads that
    // way on this hardware is the light multi-hit one.
    {"popcan", Material::Cardboard},

    // --- glass ---------------------------------------------------------
    {"glass", Material::Glass},
    {"glassbottle", Material::Glass},
    {"combine_glass", Material::Glass},
    {"pottery", Material::Glass},

    // --- wood ----------------------------------------------------------
    {"wood", Material::Wood},
    {"wood_box", Material::Wood},
    {"wood_crate", Material::Wood},
    {"wood_plank", Material::Wood},
    {"wood_panel", Material::Wood},
    {"wood_solid", Material::Wood},
    {"wood_furniture", Material::Wood},
    {"wood_lowdensity", Material::Wood},

    // --- masonry -------------------------------------------------------
    {"concrete", Material::Stone},
    {"concrete_block", Material::Stone},
    {"rock", Material::Stone},
    {"boulder", Material::Stone},
    {"brick", Material::Stone},
    {"tile", Material::Stone},
    {"plaster", Material::Stone},
    {"ceiling_tile", Material::Cardboard},
    {"asphalt", Material::Stone},

    // --- soft ground ---------------------------------------------------
    {"dirt", Material::Dirt},
    {"sand", Material::Dirt},
    {"gravel", Material::Dirt},
    {"mud", Material::Dirt},
    {"grass", Material::Dirt},
    {"snow", Material::Dirt},
    {"slipperyslime", Material::Dirt},
    {"quicksand", Material::Dirt},

    // --- flesh ---------------------------------------------------------
    {"flesh", Material::Organic},
    {"bloodyflesh", Material::Organic},
    {"alienflesh", Material::Organic},
    {"antlion", Material::Organic},
    {"zombieflesh", Material::Organic},
    {"armorflesh", Material::Organic},
    {"watermelon", Material::Organic},
    {"meat", Material::Organic},

    // --- plastic and light rigid --------------------------------------
    {"plastic", Material::Plastic},
    {"plastic_box", Material::Plastic},
    {"plastic_barrel", Material::Plastic},
    {"computer", Material::Plastic},
    {"item", Material::Plastic},

    // --- soft / dead ---------------------------------------------------
    {"rubber", Material::Rubber},
    {"rubbertire", Material::Rubber},
    {"slime", Material::Rubber},
    {"foam", Material::Rubber},

    // --- light crumple -------------------------------------------------
    {"cardboard", Material::Cardboard},
    {"paper", Material::Cardboard},
    {"papercup", Material::Cardboard},
    {"cloth", Material::Cardboard},
    {"carpet", Material::Cardboard},
    {"foliage", Material::Cardboard},
};

std::string Lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

Material MaterialFromSurfaceProp(const std::string& propName) {
    if (propName.empty()) return Material::Unknown;
    const std::string s = Lower(propName);

    // Longest matching prefix wins.
    Material best = Material::Unknown;
    size_t bestLen = 0;
    for (const auto& row : kProps) {
        const size_t n = std::char_traits<char>::length(row.prefix);
        if (n <= bestLen) continue;
        if (s.compare(0, n, row.prefix) == 0) {
            best = row.material;
            bestLen = n;
        }
    }
    return best;
}

Material MaterialFromCharCode(char code) {
    // src/game/shared/decals.h. Every mapping that is not a straight synonym is
    // justified on the tactile axis it is being placed on, because a class here
    // is a claim about how something feels and not about what it is made of.
    switch (code) {
        case 'M': return Material::Metal;      // CHAR_TEX_METAL
        case 'V': return Material::Metal;      // CHAR_TEX_VENT
        case 'G': return Material::Metal;      // CHAR_TEX_GRATE
        case 'Y': return Material::Glass;      // CHAR_TEX_GLASS
        case 'W': return Material::Wood;       // CHAR_TEX_WOOD
        case 'C': return Material::Stone;      // CHAR_TEX_CONCRETE
        // Ceramic tile is hard, thin and brittle: it clacks, it does not boom.
        // That is the plastic cell, not the concrete one.
        case 'T': return Material::Plastic;    // CHAR_TEX_TILE
        case 'L': return Material::Plastic;    // CHAR_TEX_PLASTIC
        case 'P': return Material::Plastic;    // CHAR_TEX_COMPUTER
        case 'D': return Material::Dirt;       // CHAR_TEX_DIRT
        case 'N': return Material::Dirt;       // CHAR_TEX_SAND
        case 'F': return Material::Organic;    // CHAR_TEX_FLESH
        case 'B': return Material::Organic;    // CHAR_TEX_BLOODYFLESH
        case 'H': return Material::Organic;    // CHAR_TEX_ALIENFLESH
        case 'A': return Material::Organic;    // CHAR_TEX_ANTLION
        case 'E': return Material::Organic;    // CHAR_TEX_EGGSHELL
        // Water. No transient, no ring, and the hand feels resistance rather
        // than a strike - which is the soft wobbling class, not a knock.
        case 'S': return Material::Organic;    // CHAR_TEX_SLOSH
        // Leaves and undergrowth: many small light contacts rather than one.
        // That rhythm is exactly what the cardboard recipe is.
        case 'O': return Material::Cardboard;  // CHAR_TEX_FOLIAGE
        // CHAR_TEX_CLIP is a non-solid gameplay brush and CHAR_TEX_WARPSHIELD
        // is an effect surface. Neither is a thing a hand strikes, so both stay
        // Unknown rather than being given a feel they should not have.
        case 'I':                              // CHAR_TEX_CLIP
        case 'Z':                              // CHAR_TEX_WARPSHIELD
        default: return Material::Unknown;
    }
}

Material MaterialFromSource(const std::string& propName, char code) {
    const Material byName = MaterialFromSurfaceProp(propName);
    if (byName != Material::Unknown) return byName;
    return MaterialFromCharCode(code);
}

Material MaterialFromBreakFlags(int flags) {
    // Checked in order of how strongly each reads in the hand, because a
    // breakable can carry more than one flag and the first match wins.
    if (flags & 0x01) return Material::Glass;    // BREAK_GLASS
    if (flags & 0x04) return Material::Organic;  // BREAK_FLESH
    if (flags & 0x02) return Material::Metal;    // BREAK_METAL
    if (flags & 0x40) return Material::Stone;    // BREAK_CONCRETE
    if (flags & 0x08) return Material::Wood;     // BREAK_WOOD
    return Material::Unknown;
}

} // namespace psvr2
