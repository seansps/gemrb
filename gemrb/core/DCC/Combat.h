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
	RANGE_LONG = 256, // missile -1d
	// M2.3b
	ATK_HIGHER = 512, // the attacker is on higher ground: +1 melee
	ATK_INTO_MELEE = 1024, // a missile into a melee: -1 (and a miss may hit an ally)
	DEF_COVER = 2048, // the defender is behind cover: -2
	ATK_CHARGE = 4096 // a charge: +2 melee (and -2 AC until the next round)
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
	bool autoCrit = false; // a hit is a crit (the thief's backstab)
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
	NextDefense, // Parameter1 to AC until the next attack against
	// M2.3b
	Injury, // a lasting injury: Parameter1 is the DCC::Injury bit
	ActionLimit, // fewer actions: Parameter1 ACTIONS_HALVED or ACTIONS_ONE
	Fouled, // the weapon in inventory slot Parameter1 - 1 can't be used (fumble 4)
	StandCheck, // down or stuck until an ability check: Parameter1 = DC | ability << 8 | STAND_PRONE
	Wound, // damage every round until healed: Parameter1 = count | sides << 8 | (ability + 1) << 16 (1 point a round)
	Choke, // a Fort save every round vs DC Parameter1, -1d4 Sta (lethal at 0) each failure, until one passes
	Numb, // a Fort save every round vs DC Parameter1 (+1 each round); a failure paralyzes, DC 20 passed ends it
	Fleeing, // a failed morale check: panicked, runs away until this ends
	Pinned, // held in a grapple by the creature with global ID Parameter1
	Subdued // knocked out at 0 hp by subdual damage; wakes at 1 hp when this ends or on a heal
};

// lasting injuries from crits (CombatEffect::Injury, IE_DCC_INJURY); docs/rules/crits-fumbles.md
enum Injury : uint32_t {
	INJ_NONE = 0,
	INJ_WEAPON_HAND = 1, // severed: fights with the other hand (-1d), no two weapons
	INJ_WEAPON_ARM = 2, // severed: as the hand
	INJ_ARM_USELESS = 4, // the weapon arm can't be used (until healed): as the hand
	INJ_SHIELD_ARM = 8, // severed: no two weapons, a shield gives no AC, two-handed weapons at -1d
	INJ_EYE = 16, // one eye put out: -2 to missile attacks
	INJ_SMELL = 32, // no sense of smell (narrative)
	INJ_AMNESIA = 64, // forgets its spells: casting fails
	INJ_CHEST = 128, // caved-in chest: a DC 6 Fort save each fighting round or -1 Sta for good
	INJ_BOILS = 256, // 1 damage each fighting round
	INJ_INFECTED = 512, // each night a DC 10 Fort save or -1d4 Sta
	INJ_GRAVE_ROT = 1024, // each night a DC 16 Fort save or -1d4 Sta
	INJ_ROT = 2048, // each night -1 Per (lethal at 0)
	INJ_MARKED = 4096, // un-dead are drawn to it (narrative)
	INJ_CHILLS = 8192, // chattering teeth (narrative)
	INJ_BOTH_ARMS = 16384, // both arms lost: can't attack
	INJ_LEG = 32768, // a leg lost
	INJURY_COUNT = 16
};
/** "SMELL", "EYE", ... (the key in the crit tables' INJURY terms); INJ_NONE if unknown */
GEM_EXPORT Injury ParseInjury(const std::string& key);
/** a few words for the log and the record screen ("weapon hand severed") */
GEM_EXPORT const char* InjuryName(Injury injury);
/** the attack modifiers lasting injuries give (fighting with the other hand, one eye) */
GEM_EXPORT Situational InjuryModifiers(uint32_t injuries, bool missile, bool twoHanded);
/** injuries that rule out fighting with two weapons */
GEM_EXPORT bool InjuryBlocksTwoWeapons(uint32_t injuries);
/** a shield gives no AC (its +1, Table 3-3) */
GEM_EXPORT bool InjuryBlocksShield(uint32_t injuries);

// fewer actions (Crit Table II, 6 and 17): IE_DCC_ACTLIMIT bits
constexpr int ACTIONS_HALVED = 1;
constexpr int ACTIONS_ONE = 2;
/** attacks per round (doubled, like IE_NUMBEROFATTACKS) under a limit: halved (at least half an
 * attack, one every other round), or one attack at most */
GEM_EXPORT int LimitedAttacks(int doubledAttacks, int limit);

// a check to get up or free (StandCheck): the fumble's DC 16 Agility check, a dragon's spine
constexpr int STAND_PRONE = 0x10000;
constexpr int FUMBLE_STAND_DC = 16;

// battle rage (crits-fumbles.md, footnote **): Personality or Intelligence spent for +1d12 a point
struct RageSpend {
	int personality = 0;
	int intelligence = 0;
	int Total() const { return personality + intelligence; }
};
// a score never drops below this from battle rage (Ethren decision)
constexpr int RAGE_FLOOR = 3;
constexpr int RAGE_DIE = 12;
constexpr int RAGE_MAX_SETTING = 10;
/** up to wanted points, a point at a time from the higher of the two current scores, never
 * taking either below RAGE_FLOOR */
GEM_EXPORT RageSpend BattleRage(int wanted, int personality, int intelligence);

// subdual damage (combat.md "Subdual damage")
enum class Subdual : uint8_t {
	Never, // can't deal subdual damage
	Possible, // swords, axes, clubs, spears, staves: one die lower when chosen
	Always // fists and the blackjack: always subdual, the normal die
};
/** the damage die of a subdual blow: one step down the dice chain if chosen, unchanged if always */
GEM_EXPORT int SubdualDie(int sides, Subdual kind);
/** whether a blow is subdual: always, or chosen (the stance) with a weapon the attacker is trained in */
GEM_EXPORT bool SubdualBlow(Subdual kind, bool chosen, bool trained);

// untrained weapons (classes.md "Weapon training", funnel.md): -1d from level 1 up, characters only
GEM_EXPORT int UntrainedSteps(bool character, int level, bool trained);

// morale (combat.md "Morale"): monsters and NPCs, never the party
enum MoraleTrigger : uint32_t {
	MORALE_NONE = 0,
	MORALE_FIRST_DEATH = 1, // the first of the group died
	MORALE_HALF_DOWN = 2, // half the group is dead or out of the fight
	MORALE_LEADER = 4, // the group's leader died
	MORALE_ALONE_HURT = 8 // a lone creature at half its hp or less
};
struct MoraleGroup {
	int standing = 0; // members still in the fight, the one checking included
	int down = 0; // dead (recently) or out of the fight (held, unconscious, dying)
	int deadRecently = 0; // the deaths in this fight
	bool leaderDied = false;
	bool alone = false; // no other member near
	bool halfHurt = false; // at half its hp or less
};
/** the triggers that fire now and haven't been checked (done) yet */
GEM_EXPORT uint32_t MoraleTriggers(const MoraleGroup& group, uint32_t done);
constexpr int MORALE_DC = 11;
// the situational modifier (-4..+4) from the CRE's morale (10 is neutral)
GEM_EXPORT int MoraleModifier(int ieMorale);
/** d20 + Will + modifier against the DC (11, higher with magic): true stays and fights */
GEM_EXPORT bool MoraleHolds(int natural, int will, int modifier, int dc = MORALE_DC);

// grappling (combat.md "Grappling")
/** each side adds the higher of its Str and Agi modifiers; monsters their HD */
GEM_EXPORT int GrappleModifier(bool character, int strMod, int agiMod, int hitDice);
/** the larger side's bonus by size ratio: x2 +4, x3 +8, x4 +16, doubling on; 0 for less than x2 */
GEM_EXPORT int GrappleSizeBonus(int largerSize, int smallerSize);
/** the attacker wins only by beating the defender's total (a tie fails, Ethren decision) */
GEM_EXPORT bool GrappleWins(int attackerTotal, int defenderTotal);

// the thief's backstab (classes.md): from behind or against an unaware target, the backstab
// bonus to the attack, and a hit is a crit on the thief's table
GEM_EXPORT bool BackstabApplies(bool thief, int level, bool fromBehind, bool unaware, bool immune);

// surprise (combat.md "Round sequence"): a creature that isn't in a fight yet and doesn't notice
// its attacker loses its first round
GEM_EXPORT bool Surprised(bool inFight, bool aware);

// charge (combat.md "Movement"): +2 to attack, -2 AC until the next round; a run-up of half the speed
constexpr int CHARGE_ATTACK = 2;
constexpr int CHARGE_AC = -2;
GEM_EXPORT bool ChargeRunUp(int movedFeet, int speedFeet);

// higher ground (Table 4-1, +1 melee): the attacker two steps up the area's height map (-7..+7) from the defender
constexpr int HIGHER_GROUND_STEP = 2;
GEM_EXPORT bool HigherGround(int attackerElevation, int defenderElevation);

// withdrawal (combat.md "Movement"): leaving the reach of a foe it is fighting in melee gives that
// foe a free attack, if the foe can attack and the engagement is recent
GEM_EXPORT bool WithdrawalProvokes(bool engaged, bool wasInReach, bool inReachNow, bool foeCanAttack);

// the combat options a character declares (IE_DCC_COMBATOPT, saved): one-offs for the next melee
// attack, and a stance
enum CombatOption : uint32_t {
	OPT_CHARGE = 1, // the next melee attack is a charge
	OPT_GRAPPLE = 2, // the next melee attack is a grapple instead
	OPT_SUBDUAL = 4 // subdual blows where the weapon allows it
};

// the dwarf's shield bash (classes.md): one a round, d14 + deed die, 1d3 damage
constexpr int SHIELD_BASH_DIE = 14;
constexpr int SHIELD_BASH_DAMAGE = 3;

// firing into melee and pinned creatures (combat.md): a miss has a 50% chance (1 on a d2) to go
// to an ally in the melee instead, rolled again against that ally's AC
constexpr int STRAY_DIE = 2;

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
	// M2.3b
	unsigned int target = 0; // global ID of the creature attacked (free attacks and stray shots strike others)
	bool backstab = false;
	bool subdual = false;
	bool bash = false; // the dwarf's shield bash
	bool free = false; // a free attack (a crit's, withdrawal, cleave, a stray shot)
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
