// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Combat.h
 * DCC combat rules as pure functions (docs/rules/combat.md, crits-fumbles.md,
 * equipment.md; docs/rules/open-questions.md wins where they differ): the
 * attack roll with its die, modifiers, threat range and two-weapon rules,
 * damage, crit and fumble rolls, initiative within the IE round, bleeding
 * out and recovering the body.
 *
 * The tables (crit tables, fumble table, monster crits, fumble dice by armor,
 * two-weapon dice) are in DCC/CombatTables.h; the engine glue lives in
 * Scriptable/ActorDCCCombat.cpp.
 */

#ifndef DCC_COMBAT_H
#define DCC_COMBAT_H

#include "exports.h"

#include "DCC/Character.h"

#include <cstdint>
#include <string>

namespace GemRB {
namespace DCC {

// Table 4-1 conditions the engine can tell (combat.md); the others are TODO
enum AttackConditions : uint32_t {
	ATK_NONE = 0,
	ATK_INVISIBLE = 1, // the attacker is invisible: +2 melee
	ATK_ENTANGLED = 2, // the attacker is entangled: -1d
	ATK_UNTRAINED = 4, // an untrained weapon: -1d
	DEF_BLINDED = 8, // +2
	DEF_ENTANGLED = 16, // +1d
	DEF_HELPLESS = 32, // paralyzed, asleep, held: +1d
	DEF_PRONE = 64, // +2 melee, -2 missile
	RANGE_MEDIUM = 128, // missile -2
	RANGE_LONG = 256 // missile -1d
};

struct Situational {
	int flat = 0;
	int dieSteps = 0;
};
/** the Table 4-1 modifiers for a set of conditions */
GEM_EXPORT Situational SituationalModifiers(uint32_t conditions, bool missile);

// how an attack die crits (crits-fumbles.md, combat.md Table 4-3)
enum class CritRule : uint8_t {
	Normal, // d20 and up: the natural max auto-hits and crits, the threat range crits on a hit;
	// smaller dice: only the natural max crits, and only on a hit
	MaxOnHit, // only the natural max crits, and only if it hits (two weapons, Agi 16-17)
	MaxAutoHit, // the natural max auto-hits and crits (halflings with two weapons)
	None // never crits (two weapons, Agi 15 or less, and every off hand)
};

struct AttackInput {
	int actionDie = 20; // sides, before dieSteps
	int dieSteps = 0; // -1d, +1d along the dice chain
	int modifier = 0; // every flat bonus: attack bonus or deed, ability mod, augur, Table 4-1, effects
	int targetAC = 10;
	int threat = 20; // lowest natural roll that threatens on a d20 (warriors 19, 18, 17)
	CritRule critRule = CritRule::Normal;
	bool fumbles = true; // a natural 1 fumbles; two weapons only fumble when both dice show 1
	bool cursed = false; // Luck 0: every roll fails and nothing crits (a natural 1 still fumbles)
};

/** the die an attack rolls: the action die moved along the dice chain */
GEM_EXPORT int AttackDie(int actionDie, int dieSteps);
/** the lowest natural roll that crits on a die: [max - (20 - threat), max] from d20 up, only the max below */
GEM_EXPORT int ThreatOnDie(int threat, int dieSides);
/** resolves an attack: natural is the roll on AttackDie(actionDie, dieSteps). A natural 1 always misses
 * (and fumbles if allowed); success means a hit, critical a crit */
GEM_EXPORT RollResult ResolveAttack(const AttackInput& in, int natural);

/** damage of a hit: the rolled dice + bonuses (Str mod, deed, ...), at least 1 */
GEM_EXPORT int AttackDamage(int rolled, int bonus);

/** the crit table roll: the crit dice + the class's fixed add + the attacker's Luck mod (+ the
 * warrior's arm augur) - the defender's Luck mod (a PC hit by a monster) */
GEM_EXPORT int CritRoll(int rolled, int critAdd, int attackerLuck, int defenderLuck);
/** the fumble table roll: the fumble die - the Luck mod (+ the broken star augur); lower is better */
GEM_EXPORT int FumbleRoll(int rolled, int luckMod);
/** the fumble die for the armor and shield worn (0: none): the larger die; d4 with neither */
GEM_EXPORT int FumbleDie(int armorDie, int shieldDie);
constexpr int UNARMORED_FUMBLE_DIE = 4;

// initiative (combat.md "Initiative", Ethren RTwP mapping in docs/architecture/rules-dcc.md)
/** d16 with a two-handed weapon, else d20 */
GEM_EXPORT int InitiativeDie(bool twoHanded);
struct InitiativeEntry {
	int total = 0; // die + initiative modifier
	int agility = 0; // the Agility score, for ties
	int hitDice = 0; // then the HD (level for characters)
};
/** true if a acts before b: higher total, then higher Agility score, then higher HD */
GEM_EXPORT bool InitiativeBefore(const InitiativeEntry& a, const InitiativeEntry& b);
// the totals that map onto the round: TOP and above swing at the start, TOP - SPAN and below at the end
constexpr int INITIATIVE_TOP = 25;
constexpr int INITIATIVE_SPAN = 30;
/** ticks into the round of the first attack: higher initiative swings earlier (ties: Agility, HD);
 * bottom (a crit) puts the creature last. Spread over roundTicks / attacksPerRound like the stock speed factor */
GEM_EXPORT int InitiativeDelay(const InitiativeEntry& entry, int roundTicks, int attacksPerRound, bool bottom = false);

// the kinds of the DCC:Combat effect (Parameter2; ethren effects.ids 0x1c1)
enum class CombatEffect : uint8_t {
	Bleed, // bleeding out at 0 hp; death when it runs out, a heal stops it
	Doomed, // a crit's "dies in N rounds"; death when it runs out, a heal stops it
	NoAttack, // can't attack
	Prone, // lying down: can't attack, attackers +2 melee / -2 missile
	AttackMod, // Parameter1 to attack rolls
	ACMod, // Parameter1 to AC
	InitBottom, // swings last in the round
	RollMod, // Parameter1 to every d20 roll (attacks, saves, checks)
	NextAttack, // Parameter1 to the next attack roll only
	NextDefense // Parameter1 to AC until the next attack against
};

// one attack roll as it happened (the last attack action keeps one per hand, for the log and tests)
struct AttackRecord {
	int serial = 0; // counts the attacker's attack actions
	int hand = 0; // 0 main, 1 off hand
	int die = 20;
	int natural = 0;
	int deed = 0; // the deed die result (warriors and dwarves), 0: none
	int modifier = 0;
	int total = 0;
	int ac = 0;
	bool hit = false;
	bool crit = false;
	bool fumble = false;
	bool cursed = false;
	int damage = 0;
	int critRoll = 0; // after Luck
	int critTable = 0; // CritTableID
	int fumbleDie = 0;
	int fumbleRoll = 0; // after Luck
	std::string result; // the crit or fumble result text
	// M2.4
	int luckBurned = 0; // Luck burned on this roll (by the attacker, or by the halfling luckBy)
	int luckBonus = 0; // what it added to the total
	unsigned int luckBy = 0; // global ID of a halfling who burned for the attacker, 0: the attacker
	bool fumbleCancelled = false; // a warrior or dwarf burned 1 Luck to skip the fumble table
	int deedKind = 0; // DCC::Deed declared for the attack action (0: none)
	bool deedDone = false; // the deed happened on this roll (a hit with the deed die at 3+)
	std::string deedResult; // the deed table text
};

// monsters: a CRE without a DCC block stores this HD, and its IE level is its HD and attack bonus
constexpr int HD_FROM_LEVEL = 255;

// dying (combat.md "Dying: bleeding out", "Recovering the body"; open-questions.md)
/** rounds a character at 0 hp has to be healed: its level; 0 for level 0 (dead at once) */
GEM_EXPORT int BleedOutRounds(int level);
/** an ally can roll the body over within this many seconds of death */
constexpr int BODY_RECOVERY_SECONDS = 3600;
/** level 1+ only (open-questions.md), within the hour, once per death */
GEM_EXPORT bool CanRecoverBody(int level, int secondsDead, bool alreadyTried);
// the -4 to all rolls for an hour after recovering the body
constexpr int BODY_RECOVERY_PENALTY = -4;

}
}

#endif
