// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Luck.h
 * Burning Luck as pure functions (docs/rules/luck.md, combat.md "Luck in
 * combat", classes.md; the RTwP policy in docs/rules/open-questions.md):
 * what a point of Luck is worth by class, how many points a character's
 * burn policy spends on a roll, which rolls are "key" (the game pauses and
 * asks), and the thief's and halfling's nightly recovery.
 *
 * Burned Luck is Luck loss (IE_DCC_LOSS_LCK): the score (IE_DCC_LUCK base) is
 * the natural maximum and the current score is the score minus the loss, so
 * the current Luck modifier drops with a burn and the frozen augur modifier
 * doesn't. The engine glue is in Scriptable/ActorDCCLuck.cpp.
 */

#ifndef DCC_LUCK_H
#define DCC_LUCK_H

#include "exports.h"

#include "DCC/Character.h"

#include <cstdint>

namespace GemRB {
namespace DCC {

// a character's burn policy (IE_DCC_BURNPOLICY, open-questions.md)
enum class BurnPolicy : uint8_t {
	Never, // never burns on its own (a prompt can still offer it)
	AvoidDisaster, // only to avoid death (a failed save vs death) or, for warriors and dwarves, a fumble
	MissToHit, // also turns any failed roll into a success, as long as Luck stays at the threshold or above
	count
};
constexpr int BURN_POLICY_COUNT = static_cast<int>(BurnPolicy::count);

// IE_DCC_LUCKFLAGS
enum LuckFlags : uint32_t {
	LUCK_NONE = 0,
	LUCK_PAUSE = 1, // pause and ask on key rolls (on by default)
	LUCK_ALLIES = 2 // halflings: burn for allies in sight with the same policy (on by default)
};

// what a character without the settings stored (an older or a missing CRE block) uses
constexpr BurnPolicy DEFAULT_BURN_POLICY = BurnPolicy::AvoidDisaster;
constexpr int DEFAULT_BURN_THRESHOLD = 10;
constexpr uint32_t DEFAULT_LUCK_FLAGS = LUCK_PAUSE | LUCK_ALLIES;

struct LuckPolicy {
	BurnPolicy policy = DEFAULT_BURN_POLICY;
	int threshold = DEFAULT_BURN_THRESHOLD; // MissToHit: Luck never goes below this
	uint32_t flags = DEFAULT_LUCK_FLAGS;
};

// why a burn would help
enum class BurnReason : uint8_t {
	Miss, // a failed roll (an attack that misses, a failed save or check)
	Death, // a failed roll that kills: a save vs death
	Fumble, // a warrior's or dwarf's fumble: 1 point cancels the fumble table roll
	count
};

// what one point of Luck adds to a roll
struct BurnValue {
	int perPoint = 1; // +1, a halfling's +2
	int luckDie = 0; // a thief rolls this die per point instead (0: none)
	bool IsDie() const { return luckDie > 0; }
};
/** the value of a point for a class (ClassID) and level: thieves of level 1+ roll their luck die
 * (IE_DCC_LUCKDIE), halflings of level 1+ get +2 (also for an ally), anyone else +1 */
GEM_EXPORT BurnValue BurnValueFor(int classID, int level, int luckDie);
/** the bonus of burning points: points x perPoint, or points luck dice rolled with roller */
GEM_EXPORT int BurnBonus(const BurnValue& value, int points, const Roller& roller);
/** the fewest points expected to cover gap (> 0): exact for flat values, by the die's average for
 * a thief (rounded up), so a thief may still fall short */
GEM_EXPORT int PointsToCover(const BurnValue& value, int gap);
/** the most a burn can add with points (a thief's dice all at their maximum) */
GEM_EXPORT int MaxBurnBonus(const BurnValue& value, int points);

/** warriors and dwarves of level 1+ may burn 1 Luck to cancel a fumble table roll (combat.md);
 * nobody else burns on fumbles */
GEM_EXPORT bool CanCancelFumble(int classID, int level);
constexpr int FUMBLE_CANCEL_COST = 1;

/** the points a policy burns on its own for a roll that falls short by gap (> 0; ignored for a
 * fumble) with this much Luck. Never below 1 Luck on its own (0 is cursed by fortune); a miss
 * also never below the threshold; nothing if the burn can't (be expected to) do it */
GEM_EXPORT int PolicyBurn(const LuckPolicy& policy, BurnReason reason, int gap, int luck, const BurnValue& value);

// the longest a Luck prompt waits (ms) before the policy's answer counts; PromptTimeout may be shorter
constexpr int LUCK_PROMPT_TIMEOUT = 15000;

/** a key roll pauses the game and asks the owner (with LUCK_PAUSE): saves vs death, fumbles a
 * burn can cancel, and misses that a burn would turn into a crit (the threat range decides) */
GEM_EXPORT bool IsKeyRoll(BurnReason reason, bool wouldCrit);

/** Luck a thief or halfling of level 1+ gets back each night: its level (luck.md); 0 for anyone else */
GEM_EXPORT int NightlyLuckRecovery(int classID, int level);
/** the Luck loss left after a night: recovered up to the natural maximum (loss 0), never below */
GEM_EXPORT int LuckLossAfterNight(int classID, int level, int loss);

}
}

#endif
