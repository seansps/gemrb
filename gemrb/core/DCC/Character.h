// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Character.h
 * DCC character rules as pure functions: current ability scores and ability
 * loss, the states at 0, healing loss on rest, hit points per level, save,
 * check and attack roll resolution, ascending AC, initiative and speed.
 *
 * The spec is docs/rules/ (abilities.md, luck.md, saves.md, combat.md
 * "Ability score at 0"); docs/rules/open-questions.md wins where they differ.
 * The engine glue lives in Actor (CreateDerivedStatsDCC, RefreshDCCStats).
 */

#ifndef DCC_CHARACTER_H
#define DCC_CHARACTER_H

#include "exports.h"

#include <array>
#include <cstdint>
#include <functional>

namespace GemRB {
namespace DCC {

// the six DCC abilities, in the order DCC rolls them (abilities.md, Generation)
enum class Ability : uint8_t {
	Str,
	Agi,
	Sta,
	Per,
	Int,
	Luck,
	count
};
constexpr int ABILITY_COUNT = static_cast<int>(Ability::count);
using AbilityScores = std::array<int, ABILITY_COUNT>;

/** the score after ability loss: score - loss, never below 0 */
GEM_EXPORT int CurrentScore(int score, int loss);

// states at 0 (combat.md, "Ability score at 0", Ethren design proposal)
enum StateFlags : uint32_t {
	STATE_NONE = 0,
	STATE_IMMOBILE = 1, // Str or Agi at 0: can't move or attack, helpless
	STATE_UNCONSCIOUS = 2, // Sta at 0: like sleep, but damage doesn't wake them
	STATE_WITLESS = 4, // Int or Per at 0: can't act, cast or talk
	STATE_CURSED = 8 // Luck at 0: every d20 roll fails and none crits
};
// the states that take a character out of action (a party of these and the dead is wiped)
constexpr uint32_t STATES_HELD = STATE_IMMOBILE | STATE_UNCONSCIOUS | STATE_WITLESS;

/** the states for a set of current scores; only abilities with apply[i] set are looked at */
GEM_EXPORT uint32_t StatesAtZero(const AbilityScores& current, const std::array<bool, ABILITY_COUNT>& apply);
GEM_EXPORT uint32_t StatesAtZero(const AbilityScores& current);
inline bool IsHeld(uint32_t states) { return (states & STATES_HELD) != 0; }

/** points of loss a night's rest heals: 1, or 2 with bed rest (inn) */
GEM_EXPORT int RestHealAmount(bool bedRest);
/** the loss left after a night's rest; Luck loss never heals */
GEM_EXPORT int LossAfterRest(Ability ability, int loss, bool bedRest);

/** hit points for one level: the die roll + Sta mod + augur, at least 1 */
GEM_EXPORT int LevelHP(int roll, int staminaMod, int augurBonus);
using Roller = std::function<int(int sides)>;
/** rolls the hit die with roller (1..sides) and applies LevelHP */
GEM_EXPORT int RollLevelHP(int hitDie, int staminaMod, int augurBonus, const Roller& roller);

// saves (saves.md, abilities.md)
enum class Save : uint8_t {
	Fort,
	Ref,
	Will,
	count
};
constexpr int SAVE_COUNT = static_cast<int>(Save::count);
/** Fort: Sta, Ref: Agi, Will: Per */
GEM_EXPORT Ability SaveAbility(Save save);
/** class bonus + ability mod + augur + anything else (effects) */
GEM_EXPORT int SaveTotal(int classBonus, int abilityMod, int augurBonus, int other);
// the DC of a saving throw that nothing else sets (stock IE effects): 10 + spell level
// Ethren decision until M2.5 gives spells their check total as the DC
constexpr int DEFAULT_SAVE_DC = 10;
GEM_EXPORT int DefaultSaveDC(int spellLevel);

// d20 roll resolution
enum class RollKind : uint8_t {
	Save, // d20 + save total >= DC; no natural 1/20 rule (saves.md, Ethren decision)
	Check, // ability and skill checks: die + mod >= DC, no natural rules
	Attack, // natural 1 misses (fumble), natural max hits and crits, threat range crits on a hit
	SpellCheck // natural 1 always fails (fumble); the rest is M2.5
};

struct RollResult {
	int natural = 0;
	int total = 0;
	int target = 0;
	bool success = false;
	bool critical = false;
	bool fumble = false;
	bool cursed = false; // failed because of a Luck of 0
};

/** resolves a roll of natural on a die with dieSides sides against target (DC or AC);
 * threat is the lowest natural roll that crits on an attack (0: only the max).
 * cursed (Luck 0) makes every roll fail and stops crits, but a natural 1 is still a fumble */
GEM_EXPORT RollResult ResolveRoll(RollKind kind, int natural, int dieSides, int modifier, int target, bool cursed, int threat = 0);
/** a Luck check: d20 <= the current Luck score */
GEM_EXPORT RollResult ResolveLuckCheck(int natural, int luckScore, bool cursed);

/** ascending AC: 10 + armor/shield/magic + Agi mod + augur */
GEM_EXPORT int ArmorClass(int armorShieldMagic, int agilityMod, int augurBonus);
/** the armor, shield and magic bonus that an IE descending AC stands for: 10 - AC (natural 10: +0, leather's 8: +2) */
GEM_EXPORT int BonusFromIEAC(int ieDescendingAC);

/** initiative modifier: Agi mod + warrior level (warriors only) + augur */
GEM_EXPORT int InitiativeMod(int agilityMod, int warriorLevel, int augurBonus);
/** base speed in feet: 20 for dwarves and halflings, 30 otherwise; the augur adds 5 per point */
GEM_EXPORT int BaseSpeed(bool shortRace);
GEM_EXPORT int Speed(int baseSpeed, int augurBonus);

/** IE alignment (alignmen.ids, high nibble 1 lawful, 2 neutral, 3 chaotic) to 0 lawful, 1 chaotic, 2 neutral (thief paths) */
enum class Alignment : uint8_t {
	Lawful,
	Chaotic,
	Neutral
};
GEM_EXPORT Alignment AlignmentFromIE(unsigned int ieAlignment);

}
}

#endif
