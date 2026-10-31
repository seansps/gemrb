// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Combat.h"

#include "DCC/DiceChain.h"

#include <algorithm>

namespace GemRB {
namespace DCC {

Situational SituationalModifiers(uint32_t conditions, bool missile)
{
	Situational mods;
	if ((conditions & ATK_INVISIBLE) && !missile) mods.flat += 2;
	if (conditions & ATK_ENTANGLED) mods.dieSteps -= 1;
	if (conditions & ATK_UNTRAINED) mods.dieSteps -= 1;
	if (conditions & DEF_BLINDED) mods.flat += 2;
	if (conditions & DEF_ENTANGLED) mods.dieSteps += 1;
	if (conditions & DEF_HELPLESS) mods.dieSteps += 1;
	if (conditions & DEF_PRONE) mods.flat += missile ? -2 : 2;
	if ((conditions & ATK_HIGHER) && !missile) mods.flat += 1;
	if ((conditions & ATK_CHARGE) && !missile) mods.flat += CHARGE_ATTACK;
	if (conditions & DEF_COVER) mods.flat -= 2;
	if ((conditions & ATK_INTO_MELEE) && missile) mods.flat -= 1;
	if (missile) {
		if (conditions & RANGE_LONG) {
			mods.dieSteps -= 1;
		} else if (conditions & RANGE_MEDIUM) {
			mods.flat -= 2;
		}
	}
	return mods;
}

int AttackDie(int actionDie, int dieSteps)
{
	if (actionDie <= 0) actionDie = 20;
	return dieSteps ? DiceChain::Shift(actionDie, dieSteps) : actionDie;
}

int ThreatOnDie(int threat, int dieSides)
{
	if (threat <= 0 || threat > 20) threat = 20;
	if (dieSides < 20) return dieSides;
	return std::max(2, dieSides - (20 - threat));
}

RollResult ResolveAttack(const AttackInput& in, int natural)
{
	int die = AttackDie(in.actionDie, in.dieSteps);
	RollResult result;
	result.natural = natural;
	result.total = natural + in.modifier;
	result.target = in.targetAC;
	result.cursed = in.cursed;

	bool beats = result.total >= in.targetAC;
	bool max = natural >= die;
	if (natural <= 1) {
		// a natural 1 always misses
		result.fumble = in.fumbles;
		result.success = false;
	} else {
		switch (in.critRule) {
			case CritRule::Normal:
				if (die >= 20 && max) {
					result.success = true;
					result.critical = true;
				} else {
					result.success = beats;
					result.critical = beats && natural >= ThreatOnDie(in.threat, die);
				}
				break;
			case CritRule::MaxOnHit:
				result.success = beats;
				result.critical = beats && max;
				break;
			case CritRule::MaxAutoHit:
				result.success = beats || max;
				result.critical = max;
				break;
			case CritRule::None:
			default:
				result.success = beats;
				break;
		}
	}

	// a backstab: any hit is a crit
	if (in.autoCrit && result.success) result.critical = true;
	if (in.cursed) {
		result.success = false;
		result.critical = false;
	}
	return result;
}

int AttackDamage(int rolled, int bonus)
{
	return std::max(1, rolled + bonus);
}

int CritRoll(int rolled, int critAdd, int attackerLuck, int defenderLuck)
{
	return rolled + critAdd + attackerLuck - defenderLuck;
}

int FumbleRoll(int rolled, int luckMod)
{
	return rolled - luckMod;
}

int FumbleDie(int armorDie, int shieldDie)
{
	int die = std::max(armorDie, shieldDie);
	return die > 0 ? die : UNARMORED_FUMBLE_DIE;
}

int InitiativeDie(bool twoHanded)
{
	return twoHanded ? 16 : 20;
}

bool InitiativeBefore(const InitiativeEntry& a, const InitiativeEntry& b)
{
	if (a.total != b.total) return a.total > b.total;
	if (a.agility != b.agility) return a.agility > b.agility;
	return a.hitDice > b.hitDice;
}

int InitiativeDelay(const InitiativeEntry& entry, int roundTicks, int attacksPerRound, bool bottom)
{
	// a position on a fine scale: the total first, then the Agility score, then the HD
	constexpr int fine = 32;
	constexpr int last = (INITIATIVE_SPAN + 1) * fine * fine - 1;
	int position = last;
	if (!bottom) {
		int total = std::min(std::max(entry.total, INITIATIVE_TOP - INITIATIVE_SPAN), INITIATIVE_TOP);
		int agility = std::min(std::max(entry.agility, 0), fine - 1);
		int hitDice = std::min(std::max(entry.hitDice, 0), fine - 1);
		position = ((INITIATIVE_TOP - total) * fine + (fine - 1 - agility)) * fine + (fine - 1 - hitDice);
	}
	if (attacksPerRound < 1) attacksPerRound = 1;
	return static_cast<int>(static_cast<long long>(roundTicks) * position / ((last + 1) * static_cast<long long>(attacksPerRound)));
}

int BleedOutRounds(int level)
{
	return std::max(0, level);
}

bool CanRecoverBody(int level, int secondsDead, bool alreadyTried)
{
	return level >= 1 && !alreadyTried && secondsDead >= 0 && secondsDead <= BODY_RECOVERY_SECONDS;
}


// ---------------------------------------------------------------------------
// M2.3b

static const std::pair<Injury, const char*> injuryKeys[] = {
	{ INJ_WEAPON_HAND, "HAND" }, { INJ_WEAPON_ARM, "ARM" }, { INJ_ARM_USELESS, "ARMUSELESS" }, { INJ_SHIELD_ARM, "SHIELDARM" },
	{ INJ_EYE, "EYE" }, { INJ_SMELL, "SMELL" }, { INJ_AMNESIA, "AMNESIA" }, { INJ_CHEST, "CHEST" }, { INJ_BOILS, "BOILS" },
	{ INJ_INFECTED, "INFECTED" }, { INJ_GRAVE_ROT, "GRAVEROT" }, { INJ_ROT, "ROT" }, { INJ_MARKED, "MARKED" },
	{ INJ_CHILLS, "CHILLS" }, { INJ_BOTH_ARMS, "BOTHARMS" }, { INJ_LEG, "LEG" }
};

Injury ParseInjury(const std::string& key)
{
	for (const auto& entry : injuryKeys) {
		if (key == entry.second) return entry.first;
	}
	return INJ_NONE;
}

const char* InjuryName(Injury injury)
{
	switch (injury) {
		case INJ_WEAPON_HAND:
			return "weapon hand severed";
		case INJ_WEAPON_ARM:
			return "weapon arm severed";
		case INJ_ARM_USELESS:
			return "weapon arm useless";
		case INJ_SHIELD_ARM:
			return "shield arm severed";
		case INJ_EYE:
			return "blind in one eye";
		case INJ_SMELL:
			return "no sense of smell";
		case INJ_AMNESIA:
			return "amnesia, can't cast";
		case INJ_CHEST:
			return "chest caved in";
		case INJ_BOILS:
			return "grave boils";
		case INJ_INFECTED:
			return "infected wound";
		case INJ_GRAVE_ROT:
			return "grave rot";
		case INJ_ROT:
			return "rotting flesh";
		case INJ_MARKED:
			return "marked for the un-dead";
		case INJ_CHILLS:
			return "grave chills";
		case INJ_BOTH_ARMS:
			return "both arms lost";
		case INJ_LEG:
			return "a leg lost";
		default:
			return "injured";
	}
}

Situational InjuryModifiers(uint32_t injuries, bool missile, bool twoHanded)
{
	Situational mods;
	// the weapon arm or hand is gone or useless: the other hand fights, one die lower
	if (injuries & (INJ_WEAPON_HAND | INJ_WEAPON_ARM | INJ_ARM_USELESS)) {
		mods.dieSteps -= 1;
	} else if ((injuries & INJ_SHIELD_ARM) && twoHanded) {
		// a two-handed weapon swung one-handed
		mods.dieSteps -= 1;
	}
	if ((injuries & INJ_EYE) && missile) mods.flat -= 2;
	return mods;
}

bool InjuryBlocksTwoWeapons(uint32_t injuries)
{
	return injuries & (INJ_WEAPON_HAND | INJ_WEAPON_ARM | INJ_ARM_USELESS | INJ_SHIELD_ARM | INJ_BOTH_ARMS);
}

bool InjuryBlocksShield(uint32_t injuries)
{
	return injuries & (INJ_SHIELD_ARM | INJ_BOTH_ARMS);
}

int LimitedAttacks(int doubledAttacks, int limit)
{
	if (limit & ACTIONS_ONE) doubledAttacks = std::min(doubledAttacks, 2);
	if (limit & ACTIONS_HALVED) doubledAttacks = std::max(1, doubledAttacks / 2);
	return doubledAttacks;
}

RageSpend BattleRage(int wanted, int personality, int intelligence)
{
	RageSpend spend;
	for (int i = 0; i < wanted; ++i) {
		int per = personality - spend.personality;
		int intel = intelligence - spend.intelligence;
		if (per >= intel && per > RAGE_FLOOR) {
			++spend.personality;
		} else if (intel > RAGE_FLOOR) {
			++spend.intelligence;
		} else if (per > RAGE_FLOOR) {
			++spend.personality;
		} else {
			break;
		}
	}
	return spend;
}

int SubdualDie(int sides, Subdual kind)
{
	if (kind != Subdual::Possible) return sides;
	return DiceChain::StepDown(sides);
}

bool SubdualBlow(Subdual kind, bool chosen, bool trained)
{
	if (kind == Subdual::Always) return true;
	return kind == Subdual::Possible && chosen && trained;
}

int UntrainedSteps(bool character, int level, bool trained)
{
	// level 0 is trained in its occupation's weapon and the last one it fought with (funnel.md)
	return character && level > 0 && !trained ? -1 : 0;
}

uint32_t MoraleTriggers(const MoraleGroup& group, uint32_t done)
{
	uint32_t triggers = MORALE_NONE;
	if (group.deadRecently >= 1) triggers |= MORALE_FIRST_DEATH;
	int total = group.standing + group.down;
	if (total > 1 && group.down * 2 >= total) triggers |= MORALE_HALF_DOWN;
	if (group.leaderDied) triggers |= MORALE_LEADER;
	if (group.alone && group.halfHurt) triggers |= MORALE_ALONE_HURT;
	return triggers & ~done;
}

int MoraleModifier(int ieMorale)
{
	return std::min(4, std::max(-4, ieMorale - 10));
}

bool MoraleHolds(int natural, int will, int modifier, int dc)
{
	return natural + will + modifier >= dc;
}

int GrappleModifier(bool character, int strMod, int agiMod, int hitDice)
{
	return character ? std::max(strMod, agiMod) : hitDice;
}

int GrappleSizeBonus(int largerSize, int smallerSize)
{
	if (smallerSize <= 0 || largerSize < 2 * smallerSize) return 0;
	int ratio = largerSize / smallerSize;
	int bonus = 4;
	for (int i = 2; i < ratio && bonus < (1 << 20); ++i) {
		bonus *= 2;
	}
	return bonus;
}

bool GrappleWins(int attackerTotal, int defenderTotal)
{
	return attackerTotal > defenderTotal;
}

bool BackstabApplies(bool thief, int level, bool fromBehind, bool unaware, bool immune)
{
	return thief && level >= 1 && !immune && (fromBehind || unaware);
}

bool Surprised(bool inFight, bool aware)
{
	return !inFight && !aware;
}

bool ChargeRunUp(int movedFeet, int speedFeet)
{
	return movedFeet * 2 >= speedFeet;
}

bool HigherGround(int attackerElevation, int defenderElevation)
{
	return attackerElevation - defenderElevation >= HIGHER_GROUND_STEP;
}

bool WithdrawalProvokes(bool engaged, bool wasInReach, bool inReachNow, bool foeCanAttack)
{
	return engaged && wasInReach && !inReachNow && foeCanAttack;
}

}
}
