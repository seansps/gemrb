// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC combat rules: the attack roll (totals, threat ranges, smaller and bigger
// dice, cursed by fortune), Table 4-1, damage, crit and fumble rolls, fumble dice
// from armor, initiative order and its spread over the round, bleeding out and
// recovering the body. docs/rules/combat.md, crits-fumbles.md, equipment.md

#include "../../../core/DCC/Combat.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

static AttackInput Attack(int modifier, int ac, int threat = 20)
{
	AttackInput in;
	in.modifier = modifier;
	in.targetAC = ac;
	in.threat = threat;
	return in;
}

TEST(DCC_Combat, AttackTotals)
{
	// d20 14 + attack 1 + Str 1 = 16 vs AC 12: a hit; 10 + 2 = 12 vs 12 hits (>=), 9 + 2 misses
	RollResult roll = ResolveAttack(Attack(2, 12), 14);
	EXPECT_EQ(roll.total, 16);
	EXPECT_TRUE(roll.success);
	EXPECT_FALSE(roll.critical);
	EXPECT_TRUE(ResolveAttack(Attack(2, 12), 10).success);
	EXPECT_FALSE(ResolveAttack(Attack(2, 12), 9).success);
	// negative modifiers lower the total
	EXPECT_EQ(ResolveAttack(Attack(-3, 10), 12).total, 9);
}

TEST(DCC_Combat, NaturalOneAndTwenty)
{
	// a natural 1 misses and fumbles, whatever the total
	RollResult one = ResolveAttack(Attack(30, 5), 1);
	EXPECT_FALSE(one.success);
	EXPECT_TRUE(one.fumble);
	// a natural 20 hits and crits, whatever the AC
	RollResult twenty = ResolveAttack(Attack(0, 40), 20);
	EXPECT_TRUE(twenty.success);
	EXPECT_TRUE(twenty.critical);
}

TEST(DCC_Combat, ThreatRange)
{
	// a warrior's 19-20 crits on a 19 only if it hits (crits-fumbles.md example: AC 21)
	RollResult miss = ResolveAttack(Attack(1, 21, 19), 19);
	EXPECT_FALSE(miss.success);
	EXPECT_FALSE(miss.critical);
	RollResult hit = ResolveAttack(Attack(2, 21, 19), 19);
	EXPECT_TRUE(hit.success);
	EXPECT_TRUE(hit.critical);
	// 18 isn't in the range
	EXPECT_FALSE(ResolveAttack(Attack(10, 10, 19), 18).critical);
	EXPECT_TRUE(ResolveAttack(Attack(10, 10, 17), 17).critical);
}

TEST(DCC_Combat, ThreatOnOtherDice)
{
	EXPECT_EQ(ThreatOnDie(20, 20), 20);
	EXPECT_EQ(ThreatOnDie(19, 20), 19);
	EXPECT_EQ(ThreatOnDie(17, 20), 17);
	// bigger dice: the top of the die, 19-20 on a d24 is 23-24
	EXPECT_EQ(ThreatOnDie(19, 24), 23);
	EXPECT_EQ(ThreatOnDie(20, 30), 30);
	// smaller dice: only the max
	EXPECT_EQ(ThreatOnDie(19, 16), 16);
	EXPECT_EQ(ThreatOnDie(20, 14), 14);
	// a bad threat counts as 20
	EXPECT_EQ(ThreatOnDie(0, 20), 20);
}

TEST(DCC_Combat, SmallerAndBiggerDice)
{
	// a d16 action die: the max crits only on a hit, never auto-hits
	AttackInput in = Attack(0, 20);
	in.actionDie = 16;
	RollResult max = ResolveAttack(in, 16);
	EXPECT_FALSE(max.success);
	EXPECT_FALSE(max.critical);
	in.targetAC = 15;
	max = ResolveAttack(in, 16);
	EXPECT_TRUE(max.success);
	EXPECT_TRUE(max.critical);
	// a d24: 24 auto-hits and crits, 23 with threat 19 crits on a hit
	in = Attack(0, 30, 19);
	in.actionDie = 24;
	EXPECT_TRUE(ResolveAttack(in, 24).critical);
	EXPECT_FALSE(ResolveAttack(in, 23).success);
	in.targetAC = 20;
	EXPECT_TRUE(ResolveAttack(in, 23).critical);
	EXPECT_FALSE(ResolveAttack(in, 22).critical);
}

TEST(DCC_Combat, DieSteps)
{
	EXPECT_EQ(AttackDie(20, 0), 20);
	EXPECT_EQ(AttackDie(20, -1), 16);
	EXPECT_EQ(AttackDie(20, -3), 12);
	EXPECT_EQ(AttackDie(16, -1), 14);
	EXPECT_EQ(AttackDie(20, 1), 24);
	EXPECT_EQ(AttackDie(0, 0), 20);
	// the steps apply before resolving: a helpless target (+1d) is rolled on a d24
	AttackInput in = Attack(0, 30);
	in.dieSteps = 1;
	EXPECT_TRUE(ResolveAttack(in, 24).critical);
}

TEST(DCC_Combat, CritRules)
{
	AttackInput in = Attack(0, 18);
	in.actionDie = 16;
	in.critRule = CritRule::None;
	EXPECT_FALSE(ResolveAttack(in, 16).critical);
	EXPECT_FALSE(ResolveAttack(in, 16).success); // 16 vs 18
	in.critRule = CritRule::MaxOnHit;
	EXPECT_FALSE(ResolveAttack(in, 16).critical);
	in.targetAC = 16;
	EXPECT_TRUE(ResolveAttack(in, 16).critical);
	// halflings: a natural 16 auto-hits and crits
	in.targetAC = 30;
	in.critRule = CritRule::MaxAutoHit;
	EXPECT_TRUE(ResolveAttack(in, 16).success);
	EXPECT_TRUE(ResolveAttack(in, 16).critical);
	EXPECT_FALSE(ResolveAttack(in, 15).success);
}

TEST(DCC_Combat, FumbleOnlyWhereAllowed)
{
	AttackInput in = Attack(0, 10);
	in.fumbles = false; // a halfling's single 1 with two weapons
	RollResult one = ResolveAttack(in, 1);
	EXPECT_FALSE(one.success);
	EXPECT_FALSE(one.fumble);
}

TEST(DCC_Combat, CursedByFortune)
{
	AttackInput in = Attack(10, 5);
	in.cursed = true;
	RollResult roll = ResolveAttack(in, 20);
	EXPECT_FALSE(roll.success);
	EXPECT_FALSE(roll.critical);
	EXPECT_TRUE(roll.cursed);
	// a natural 1 still fumbles
	EXPECT_TRUE(ResolveAttack(in, 1).fumble);
}

TEST(DCC_Combat, Table41)
{
	Situational none = SituationalModifiers(ATK_NONE, false);
	EXPECT_EQ(none.flat, 0);
	EXPECT_EQ(none.dieSteps, 0);
	EXPECT_EQ(SituationalModifiers(ATK_INVISIBLE, false).flat, 2);
	EXPECT_EQ(SituationalModifiers(ATK_INVISIBLE, true).flat, 0);
	EXPECT_EQ(SituationalModifiers(DEF_BLINDED, true).flat, 2);
	EXPECT_EQ(SituationalModifiers(DEF_HELPLESS, false).dieSteps, 1);
	EXPECT_EQ(SituationalModifiers(DEF_ENTANGLED | DEF_HELPLESS, false).dieSteps, 2);
	EXPECT_EQ(SituationalModifiers(ATK_ENTANGLED | ATK_UNTRAINED, false).dieSteps, -2);
	EXPECT_EQ(SituationalModifiers(DEF_PRONE, false).flat, 2);
	EXPECT_EQ(SituationalModifiers(DEF_PRONE, true).flat, -2);
	EXPECT_EQ(SituationalModifiers(RANGE_MEDIUM, true).flat, -2);
	EXPECT_EQ(SituationalModifiers(RANGE_LONG, true).dieSteps, -1);
	EXPECT_EQ(SituationalModifiers(RANGE_LONG | RANGE_MEDIUM, false).dieSteps, 0); // melee ignores range
}

TEST(DCC_Combat, DamageAtLeastOne)
{
	EXPECT_EQ(AttackDamage(5, 2), 7);
	EXPECT_EQ(AttackDamage(1, -3), 1);
	EXPECT_EQ(AttackDamage(2, -2), 1);
}

TEST(DCC_Combat, CritAndFumbleRolls)
{
	// crit: dice + class add + attacker Luck - defender Luck
	EXPECT_EQ(CritRoll(7, 0, 0, 0), 7);
	EXPECT_EQ(CritRoll(15, 2, 1, 0), 18);
	EXPECT_EQ(CritRoll(4, 0, 0, 2), 2); // a lucky PC hit by a monster
	EXPECT_EQ(CritRoll(1, 0, -3, 0), -2);
	// fumble: the die - Luck, lower is better
	EXPECT_EQ(FumbleRoll(3, 0), 3);
	EXPECT_EQ(FumbleRoll(3, 2), 1);
	EXPECT_EQ(FumbleRoll(3, -1), 4);
}

TEST(DCC_Combat, FumbleDieFromArmor)
{
	EXPECT_EQ(FumbleDie(0, 0), 4); // unarmored, no shield
	EXPECT_EQ(FumbleDie(8, 0), 8);
	EXPECT_EQ(FumbleDie(0, 8), 8); // a shield only
	EXPECT_EQ(FumbleDie(12, 8), 12); // the larger die (open-questions.md)
	EXPECT_EQ(FumbleDie(16, 8), 16);
	EXPECT_EQ(FumbleDie(4, 8), 8);
}

TEST(DCC_Combat, InitiativeOrder)
{
	EXPECT_EQ(InitiativeDie(false), 20);
	EXPECT_EQ(InitiativeDie(true), 16); // two-handed weapons
	InitiativeEntry fast { 15, 10, 1 };
	InitiativeEntry slow { 12, 18, 3 };
	EXPECT_TRUE(InitiativeBefore(fast, slow));
	EXPECT_FALSE(InitiativeBefore(slow, fast));
	// ties: the Agility score, then HD
	InitiativeEntry agile { 15, 16, 1 };
	EXPECT_TRUE(InitiativeBefore(agile, fast));
	InitiativeEntry tough { 15, 10, 4 };
	EXPECT_TRUE(InitiativeBefore(tough, fast));
	EXPECT_FALSE(InitiativeBefore(fast, fast));
}

TEST(DCC_Combat, InitiativeDelay)
{
	const int round = 90;
	InitiativeEntry top { INITIATIVE_TOP + 5, 18, 5 };
	InitiativeEntry mid { 10, 12, 1 };
	InitiativeEntry low { -20, 3, 0 };
	// higher initiative swings earlier, all within the round
	int first = InitiativeDelay(top, round, 1);
	int second = InitiativeDelay(mid, round, 1);
	int third = InitiativeDelay(low, round, 1);
	EXPECT_LT(first, second);
	EXPECT_LT(second, third);
	EXPECT_GE(first, 0);
	EXPECT_LT(third, round);
	// the order follows InitiativeBefore, ties included
	InitiativeEntry agile { 10, 16, 1 };
	EXPECT_LE(InitiativeDelay(agile, round, 1), second);
	// two attacks a round spread over half the round each
	EXPECT_EQ(InitiativeDelay(mid, round, 2), InitiativeDelay(mid, round, 1) / 2);
	// a crit's "bottom of the initiative" is the last slot
	EXPECT_GE(InitiativeDelay(top, round, 1, true), third);
	EXPECT_LT(InitiativeDelay(top, round, 1, true), round);
}

TEST(DCC_Combat, BleedOutTimer)
{
	EXPECT_EQ(BleedOutRounds(0), 0); // level 0: dead
	EXPECT_EQ(BleedOutRounds(1), 1);
	EXPECT_EQ(BleedOutRounds(4), 4);
	EXPECT_EQ(BleedOutRounds(10), 10);
}

TEST(DCC_Combat, RecoverTheBody)
{
	EXPECT_TRUE(CanRecoverBody(1, 0, false));
	EXPECT_TRUE(CanRecoverBody(3, BODY_RECOVERY_SECONDS, false));
	EXPECT_FALSE(CanRecoverBody(3, BODY_RECOVERY_SECONDS + 1, false)); // too late
	EXPECT_FALSE(CanRecoverBody(0, 10, false)); // level 0 deaths are final
	EXPECT_FALSE(CanRecoverBody(2, 10, true)); // once per death
}

}
}
