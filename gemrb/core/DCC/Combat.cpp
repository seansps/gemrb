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

}
}
