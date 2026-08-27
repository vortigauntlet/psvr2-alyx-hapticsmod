// Half-Life 2 VR's own bHaptics event keys -> this project's semantic events.
//
// ===========================================================================
// THE HONEST PROBLEM THIS FILE HAS
// ===========================================================================
//
// The transport is verified: both bHaptics SDKs are WebSocket clients to
// 127.0.0.1:15881, and a game with built-in support announces every haptic
// pattern it owns in a `Register` array on connect. See core/bhaptics_listener.h.
//
// What is NOT verified is the KEY NAMES Half-Life 2 VR uses, because reading
// them needs the game. So this file must not pretend to know them.
//
// It therefore does two things and refuses to do a third:
//
//   1. Matches on lowercase SUBSTRINGS rather than exact names, because the one
//      thing that can be relied on is that a pattern for the shotgun firing
//      will have "shotgun" somewhere in its name. This is how the bHaptics
//      Alyx integration names things - its .tact files are ChamberedRound_1,
//      ClipInserted_1, DamageExplosion_1 - and the same author conventions run
//      across their whole catalogue.
//
//   2. REPORTS every key it could not classify, by name, so one play session
//      turns the unknown list into a known one. `--bhaptics-scan` exists for
//      exactly this and does nothing else.
//
//   3. It does NOT invent a mapping to make the table look complete. An
//      unmapped key produces no haptic and one line of output naming it. A
//      wrong mapping would be worse than silence: it would fire the wrong
//      sensation at the right moment, which is much harder to notice and much
//      harder to debug than nothing happening.
//
// The substring rules below are ordered most-specific first, and each one says
// what it is keying on. When the real names arrive they should REPLACE these
// with exact matches - the substring layer is scaffolding for the discovery
// phase, not the intended end state.

#pragma once

#include <string>
#include <vector>

namespace psvr2 {

// What a bHaptics key was understood to mean. Empty `event` means unmapped.
struct BhapticsMapping {
    std::string event;   // the HL2_* event to raise, or empty
    std::string params;  // its parameter string
    // Why this key matched, for the diagnostics line. Naming the rule that
    // fired is what makes a wrong guess visible instead of mysterious.
    const char* rule = nullptr;
};

// Classifies one bHaptics pattern key.
BhapticsMapping MapBhapticsKey(const std::string& key);

// The substring rules, for --bhaptics-scan to print. Exposed so the discovery
// tool can show what it WOULD match, rather than the user having to read source
// to find out why a key went unmapped.
struct BhapticsRule {
    const char* match;
    const char* event;
    const char* why;
};
const std::vector<BhapticsRule>& BhapticsRules();

} // namespace psvr2
