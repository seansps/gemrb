// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Character.h"

#include <algorithm>

namespace GemRB {
namespace DCC {

int CurrentScore(int score, int loss)
{
	return std::max(0, score - loss);
}

uint32_t StatesAtZero(const AbilityScores& current, const std::array<bool, ABILITY_COUNT>& apply)
{
	auto zero = [&current, &apply](Ability ability) {
		size_t i = static_cast<size_t>(ability);
		return apply[i] && current[i] <= 0;
	};

	uint32_t states = STATE_NONE;
	if (zero(Ability::Str) || zero(Ability::Agi)) states |= STATE_IMMOBILE;
	if (zero(Ability::Sta)) states |= STATE_UNCONSCIOUS;
	if (zero(Ability::Int) || zero(Ability::Per)) states |= STATE_WITLESS;
	if (zero(Ability::Luck)) states |= STATE_CURSED;
	return states;
}

uint32_t StatesAtZero(const AbilityScores& current)
{
	std::array<bool, ABILITY_COUNT> all;
	all.fill(true);
	return StatesAtZero(current, all);
}

int RestHealAmount(bool bedRest)
{
	return bedRest ? 2 : 1;
}

int LossAfterRest(Ability ability, int loss, bool bedRest)
{
	if (ability == Ability::Luck) return loss;
	return std::max(0, loss - RestHealAmount(bedRest));
}

int LevelHP(int roll, int staminaMod, int augurBonus)
{
	return std::max(1, roll + staminaMod + augurBonus);
}

int RollLevelHP(int hitDie, int staminaMod, int augurBonus, const Roller& roller)
{
	int roll = hitDie > 0 ? roller(hitDie) : 0;
	return LevelHP(roll, staminaMod, augurBonus);
}

Ability SaveAbility(Save save)
{
	switch (save) {
		case Save::Fort:
			return Ability::Sta;
		case Save::Ref:
			return Ability::Agi;
		default:
			return Ability::Per;
	}
}

int SaveTotal(int classBonus, int abilityMod, int augurBonus, int other)
{
	return classBonus + abilityMod + augurBonus + other;
}

int DefaultSaveDC(int spellLevel)
{
	return DEFAULT_SAVE_DC + std::max(0, spellLevel);
}

RollResult ResolveRoll(RollKind kind, int natural, int dieSides, int modifier, int target, bool cursed, int threat)
{
	RollResult result;
	result.natural = natural;
	result.total = natural + modifier;
	result.target = target;
	result.cursed = cursed;

	switch (kind) {
		case RollKind::Attack:
			result.fumble = natural == 1;
			if (result.fumble) {
				result.success = false;
			} else if (natural >= dieSides) {
				result.success = true;
				result.critical = true;
			} else {
				result.success = result.total >= target;
				result.critical = result.success && threat > 0 && natural >= threat;
			}
			break;
		case RollKind::SpellCheck:
			result.fumble = natural == 1;
			result.success = !result.fumble && result.total >= target;
			break;
		case RollKind::Save:
		case RollKind::Check:
		default:
			result.success = result.total >= target;
			break;
	}

	if (cursed) {
		result.success = false;
		result.critical = false;
	}
	return result;
}

RollResult ResolveLuckCheck(int natural, int luckScore, bool cursed)
{
	RollResult result;
	result.natural = natural;
	result.total = natural;
	result.target = luckScore;
	result.cursed = cursed;
	result.success = !cursed && natural <= luckScore;
	return result;
}

int ArmorClass(int armorShieldMagic, int agilityMod, int augurBonus)
{
	return 10 + armorShieldMagic + agilityMod + augurBonus;
}

int BonusFromIEAC(int ieDescendingAC)
{
	return 10 - ieDescendingAC;
}

int InitiativeMod(int agilityMod, int warriorLevel, int augurBonus)
{
	return agilityMod + warriorLevel + augurBonus;
}

int BaseSpeed(bool shortRace)
{
	return shortRace ? 20 : 30;
}

int Speed(int baseSpeed, int augurBonus)
{
	return baseSpeed + 5 * augurBonus;
}

Alignment AlignmentFromIE(unsigned int ieAlignment)
{
	switch ((ieAlignment >> 4) & 0xf) {
		case 1:
			return Alignment::Lawful;
		case 3:
			return Alignment::Chaotic;
		default:
			return Alignment::Neutral;
	}
}

}
}
