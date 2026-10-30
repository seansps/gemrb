// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC character rules: ability loss and the states at 0, healing on rest, hit
// points per level, saves, checks and attack rolls, AC, initiative and speed.
// docs/rules/abilities.md, saves.md, combat.md ("Ability score at 0")

#include "../../../core/DCC/Character.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

TEST(DCC_Character, CurrentScoreFloorsAtZero)
{
	EXPECT_EQ(CurrentScore(12, 0), 12);
	EXPECT_EQ(CurrentScore(12, 3), 9);
	EXPECT_EQ(CurrentScore(3, 3), 0);
	EXPECT_EQ(CurrentScore(3, 7), 0);
	EXPECT_EQ(CurrentScore(0, 0), 0);
}

TEST(DCC_Character, StatesAtZero)
{
	AbilityScores fine { 10, 10, 10, 10, 10, 10 };
	EXPECT_EQ(StatesAtZero(fine), STATE_NONE);

	auto with = [&fine](Ability ability) {
		AbilityScores scores = fine;
		scores[static_cast<size_t>(ability)] = 0;
		return StatesAtZero(scores);
	};
	EXPECT_EQ(with(Ability::Str), STATE_IMMOBILE);
	EXPECT_EQ(with(Ability::Agi), STATE_IMMOBILE);
	EXPECT_EQ(with(Ability::Sta), STATE_UNCONSCIOUS);
	EXPECT_EQ(with(Ability::Per), STATE_WITLESS);
	EXPECT_EQ(with(Ability::Int), STATE_WITLESS);
	EXPECT_EQ(with(Ability::Luck), STATE_CURSED);

	AbilityScores wrecked { 0, 0, 0, 0, 0, 0 };
	EXPECT_EQ(StatesAtZero(wrecked), STATE_IMMOBILE | STATE_UNCONSCIOUS | STATE_WITLESS | STATE_CURSED);

	// only the abilities looked at count (monsters whose CRE leaves a score at 0)
	std::array<bool, ABILITY_COUNT> onlyStr { true, false, false, false, false, false };
	EXPECT_EQ(StatesAtZero(wrecked, onlyStr), STATE_IMMOBILE);
	std::array<bool, ABILITY_COUNT> none {};
	EXPECT_EQ(StatesAtZero(wrecked, none), STATE_NONE);
}

TEST(DCC_Character, HeldStates)
{
	EXPECT_FALSE(IsHeld(STATE_NONE));
	EXPECT_TRUE(IsHeld(STATE_IMMOBILE));
	EXPECT_TRUE(IsHeld(STATE_UNCONSCIOUS));
	EXPECT_TRUE(IsHeld(STATE_WITLESS));
	// cursed by fortune can still act, so it doesn't count towards a party wipe
	EXPECT_FALSE(IsHeld(STATE_CURSED));
	EXPECT_TRUE(IsHeld(STATE_CURSED | STATE_WITLESS));
}

TEST(DCC_Character, LossHealsOnRestExceptLuck)
{
	EXPECT_EQ(RestHealAmount(false), 1);
	EXPECT_EQ(RestHealAmount(true), 2);

	for (Ability ability : { Ability::Str, Ability::Agi, Ability::Sta, Ability::Per, Ability::Int }) {
		EXPECT_EQ(LossAfterRest(ability, 3, false), 2);
		EXPECT_EQ(LossAfterRest(ability, 3, true), 1);
		EXPECT_EQ(LossAfterRest(ability, 1, true), 0);
		EXPECT_EQ(LossAfterRest(ability, 0, false), 0);
	}
	EXPECT_EQ(LossAfterRest(Ability::Luck, 3, false), 3);
	EXPECT_EQ(LossAfterRest(Ability::Luck, 3, true), 3);

	// a Stamina 3 peasant drained by 4 wakes after two nights in the field: the score
	// is back to 1 once the loss is 2 or less
	int loss = 4;
	loss = LossAfterRest(Ability::Sta, loss, false);
	EXPECT_EQ(StatesAtZero({ 10, 10, CurrentScore(3, loss), 10, 10, 10 }), STATE_UNCONSCIOUS);
	loss = LossAfterRest(Ability::Sta, loss, false);
	EXPECT_EQ(StatesAtZero({ 10, 10, CurrentScore(3, loss), 10, 10, 10 }), STATE_NONE);
}

TEST(DCC_Character, HitPointsPerLevel)
{
	EXPECT_EQ(LevelHP(4, 0, 0), 4);
	EXPECT_EQ(LevelHP(8, 2, 0), 10);
	EXPECT_EQ(LevelHP(5, -3, 0), 2);
	// at least 1 per level, whatever the Stamina
	EXPECT_EQ(LevelHP(1, -3, 0), 1);
	EXPECT_EQ(LevelHP(2, -3, -1), 1);
	// bountiful harvest adds the frozen Luck modifier each level
	EXPECT_EQ(LevelHP(6, 1, 2), 9);

	int asked = 0;
	Roller fixed = [&asked](int sides) {
		asked = sides;
		return sides / 2;
	};
	EXPECT_EQ(RollLevelHP(12, 1, 0, fixed), 7); // warrior d12 rolls 6, Sta +1
	EXPECT_EQ(asked, 12);
	EXPECT_EQ(RollLevelHP(4, -3, 0, fixed), 1); // 2 - 3, floored
	EXPECT_EQ(asked, 4);

	// every roll of a die stays within its minimum and maximum
	for (int roll = 1; roll <= 4; ++roll) {
		Roller exact = [roll](int) { return roll; };
		EXPECT_EQ(RollLevelHP(4, 3, 0, exact), roll + 3);
		EXPECT_EQ(RollLevelHP(4, -3, 0, exact), std::max(1, roll - 3));
	}
}

TEST(DCC_Character, SaveAbilities)
{
	EXPECT_EQ(SaveAbility(Save::Fort), Ability::Sta);
	EXPECT_EQ(SaveAbility(Save::Ref), Ability::Agi);
	EXPECT_EQ(SaveAbility(Save::Will), Ability::Per);
	EXPECT_EQ(SaveTotal(1, 3, 0, 0), 4);
	EXPECT_EQ(SaveTotal(0, -3, 1, 2), 0);
	EXPECT_EQ(DefaultSaveDC(0), 10);
	EXPECT_EQ(DefaultSaveDC(3), 13);
	EXPECT_EQ(DefaultSaveDC(-1), 10);
}

TEST(DCC_Character, SavesAndChecksHaveNoNaturalRules)
{
	RollResult r = ResolveRoll(RollKind::Save, 12, 20, 3, 15, false);
	EXPECT_EQ(r.total, 15);
	EXPECT_TRUE(r.success);
	r = ResolveRoll(RollKind::Save, 11, 20, 3, 15, false);
	EXPECT_FALSE(r.success);
	// a natural 20 doesn't save against a DC out of reach, a natural 1 still saves if the total is enough
	r = ResolveRoll(RollKind::Save, 20, 20, -3, 21, false);
	EXPECT_FALSE(r.success);
	EXPECT_FALSE(r.critical);
	r = ResolveRoll(RollKind::Save, 1, 20, 9, 10, false);
	EXPECT_TRUE(r.success);
	EXPECT_FALSE(r.fumble);
	r = ResolveRoll(RollKind::Check, 7, 10, 2, 9, false);
	EXPECT_TRUE(r.success);
}

TEST(DCC_Character, AttackRollNaturals)
{
	// natural 1 misses and fumbles, whatever the bonus
	RollResult r = ResolveRoll(RollKind::Attack, 1, 20, 30, 10, false);
	EXPECT_FALSE(r.success);
	EXPECT_TRUE(r.fumble);
	// natural max hits and crits, whatever the AC
	r = ResolveRoll(RollKind::Attack, 20, 20, -5, 30, false);
	EXPECT_TRUE(r.success);
	EXPECT_TRUE(r.critical);
	r = ResolveRoll(RollKind::Attack, 16, 16, 0, 30, false);
	EXPECT_TRUE(r.critical);
	// a warrior's 19 crits only on a hit
	r = ResolveRoll(RollKind::Attack, 19, 20, 0, 19, false, 19);
	EXPECT_TRUE(r.success);
	EXPECT_TRUE(r.critical);
	r = ResolveRoll(RollKind::Attack, 19, 20, 0, 25, false, 19);
	EXPECT_FALSE(r.success);
	EXPECT_FALSE(r.critical);
	r = ResolveRoll(RollKind::Attack, 18, 20, 0, 10, false, 19);
	EXPECT_TRUE(r.success);
	EXPECT_FALSE(r.critical);
	// spell checks fail on a natural 1
	r = ResolveRoll(RollKind::SpellCheck, 1, 20, 20, 10, false);
	EXPECT_FALSE(r.success);
	EXPECT_TRUE(r.fumble);
}

TEST(DCC_Character, CursedByFortuneFailsEveryRoll)
{
	for (RollKind kind : { RollKind::Save, RollKind::Check, RollKind::Attack, RollKind::SpellCheck }) {
		RollResult r = ResolveRoll(kind, 20, 20, 10, 5, true);
		EXPECT_FALSE(r.success);
		EXPECT_FALSE(r.critical);
		EXPECT_TRUE(r.cursed);
		EXPECT_EQ(r.total, 30);
	}
	// a natural 1 is still a fumble
	RollResult r = ResolveRoll(RollKind::Attack, 1, 20, 10, 5, true);
	EXPECT_TRUE(r.fumble);
	EXPECT_FALSE(ResolveLuckCheck(1, 18, true).success);
}

TEST(DCC_Character, LuckCheckRollsUnder)
{
	EXPECT_TRUE(ResolveLuckCheck(11, 11, false).success);
	EXPECT_FALSE(ResolveLuckCheck(12, 11, false).success);
	EXPECT_TRUE(ResolveLuckCheck(1, 3, false).success);
	// with a Luck of 0 nothing gets under it
	EXPECT_FALSE(ResolveLuckCheck(1, 0, false).success);
}

TEST(DCC_Character, AscendingArmorClass)
{
	// AC = 10 + armor + shield + Agi mod + magic (combat.md)
	EXPECT_EQ(ArmorClass(0, 0, 0), 10);
	EXPECT_EQ(ArmorClass(0, -2, 0), 8);
	EXPECT_EQ(ArmorClass(2 + 1, 1, 0), 14); // leather and shield, Agi 13
	EXPECT_EQ(ArmorClass(8, 3, 1), 22); // full plate, Agi 18, charmed house +1
	// the IE descending AC of the armor and magic: natural 10 is unarmored, leather sets 8
	EXPECT_EQ(BonusFromIEAC(10), 0);
	EXPECT_EQ(BonusFromIEAC(8), 2);
	EXPECT_EQ(BonusFromIEAC(2), 8);
	EXPECT_EQ(BonusFromIEAC(11), -1);
}

TEST(DCC_Character, InitiativeAndSpeed)
{
	EXPECT_EQ(InitiativeMod(0, 0, 0), 0);
	EXPECT_EQ(InitiativeMod(-3, 0, 0), -3);
	EXPECT_EQ(InitiativeMod(1, 3, 0), 4); // 3rd level warrior
	EXPECT_EQ(InitiativeMod(2, 0, -1), 1); // speed of the cobra, unlucky
	EXPECT_EQ(BaseSpeed(false), 30);
	EXPECT_EQ(BaseSpeed(true), 20);
	EXPECT_EQ(Speed(30, 2), 40); // wild child
	EXPECT_EQ(Speed(20, -1), 15);
}

TEST(DCC_Character, AlignmentPaths)
{
	EXPECT_EQ(AlignmentFromIE(0x11), Alignment::Lawful);
	EXPECT_EQ(AlignmentFromIE(0x13), Alignment::Lawful);
	EXPECT_EQ(AlignmentFromIE(0x22), Alignment::Neutral);
	EXPECT_EQ(AlignmentFromIE(0x32), Alignment::Chaotic);
	EXPECT_EQ(AlignmentFromIE(0), Alignment::Neutral);
}

}
}
