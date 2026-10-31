// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC combat completion (M2.3b): subdual and untrained weapon dice, morale triggers and
// checks, grapple totals and sizes, the thief's backstab auto-crit, surprise, charge, higher
// ground, withdrawal, battle rage, fewer actions and lasting injuries.
// docs/rules/combat.md, crits-fumbles.md, classes.md, equipment.md

#include "../../../core/DCC/Combat.h"
#include "../../../core/DCC/CombatTables.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

TEST(DCC_CombatRules, SubdualDice)
{
	// a chosen subdual blow is a die lower on the chain: longsword d8 -> d7, d10 -> d8, d4 -> d3
	EXPECT_EQ(SubdualDie(8, Subdual::Possible), 7);
	EXPECT_EQ(SubdualDie(10, Subdual::Possible), 8);
	EXPECT_EQ(SubdualDie(4, Subdual::Possible), 3);
	EXPECT_EQ(SubdualDie(3, Subdual::Possible), 3); // the bottom of the chain
	// fists and the blackjack are always subdual, with their own die
	EXPECT_EQ(SubdualDie(3, Subdual::Always), 3);
	EXPECT_EQ(SubdualDie(6, Subdual::Never), 6);
	// a blow is subdual: always, or chosen with a weapon that allows it and training in it
	EXPECT_TRUE(SubdualBlow(Subdual::Always, false, false));
	EXPECT_TRUE(SubdualBlow(Subdual::Possible, true, true));
	EXPECT_FALSE(SubdualBlow(Subdual::Possible, true, false)); // not proficient
	EXPECT_FALSE(SubdualBlow(Subdual::Possible, false, true)); // not chosen
	EXPECT_FALSE(SubdualBlow(Subdual::Never, true, true)); // a dagger or a mace
}

TEST(DCC_CombatRules, UntrainedDice)
{
	// -1d for an untrained weapon from level 1; never at level 0 (funnel.md), never for monsters
	EXPECT_EQ(UntrainedSteps(true, 1, false), -1);
	EXPECT_EQ(UntrainedSteps(true, 5, false), -1);
	EXPECT_EQ(UntrainedSteps(true, 1, true), 0);
	EXPECT_EQ(UntrainedSteps(true, 0, false), 0);
	EXPECT_EQ(UntrainedSteps(false, 3, false), 0);
	// Table 4-1 applies it: d20 -> d16
	Situational mods = SituationalModifiers(ATK_UNTRAINED, false);
	EXPECT_EQ(mods.dieSteps, -1);
	EXPECT_EQ(AttackDie(20, mods.dieSteps), 16);
}

TEST(DCC_CombatRules, WeaponClassColumns)
{
	EXPECT_EQ(WeaponClassFor(ClassID::Warrior, Alignment::Chaotic), WeaponClass::Warrior);
	EXPECT_EQ(WeaponClassFor(ClassID::Cleric, Alignment::Lawful), WeaponClass::ClericLaw);
	EXPECT_EQ(WeaponClassFor(ClassID::Cleric, Alignment::Neutral), WeaponClass::ClericNeutral);
	EXPECT_EQ(WeaponClassFor(ClassID::Cleric, Alignment::Chaotic), WeaponClass::ClericChaos);
	EXPECT_EQ(WeaponClassFor(ClassID::Level0, Alignment::Lawful), WeaponClass::count);
	EXPECT_EQ(WeaponClassFor(ClassID::None, Alignment::Lawful), WeaponClass::count);
}

TEST(DCC_CombatRules, MoraleTriggers)
{
	// the first of a group of four dies: a check
	MoraleGroup group;
	group.standing = 3;
	group.down = 1;
	group.deadRecently = 1;
	EXPECT_EQ(MoraleTriggers(group, MORALE_NONE), uint32_t(MORALE_FIRST_DEATH));
	// once only
	EXPECT_EQ(MoraleTriggers(group, MORALE_FIRST_DEATH), uint32_t(MORALE_NONE));
	// half the group down (dead or out of the fight)
	group.standing = 2;
	group.down = 2;
	group.deadRecently = 2;
	EXPECT_EQ(MoraleTriggers(group, MORALE_FIRST_DEATH), uint32_t(MORALE_HALF_DOWN));
	// the leader falls
	group.leaderDied = true;
	EXPECT_EQ(MoraleTriggers(group, MORALE_FIRST_DEATH | MORALE_HALF_DOWN), uint32_t(MORALE_LEADER));
	// a lone monster at half hp
	MoraleGroup alone;
	alone.standing = 1;
	alone.alone = true;
	alone.halfHurt = true;
	EXPECT_EQ(MoraleTriggers(alone, MORALE_NONE), uint32_t(MORALE_ALONE_HURT));
	alone.halfHurt = false;
	EXPECT_EQ(MoraleTriggers(alone, MORALE_NONE), uint32_t(MORALE_NONE));
	// a group of one isn't "half down" by itself
	MoraleGroup one;
	one.standing = 1;
	EXPECT_EQ(MoraleTriggers(one, MORALE_NONE), uint32_t(MORALE_NONE));
}

TEST(DCC_CombatRules, MoraleCheck)
{
	// d20 + Will vs 11: 11+ stands, 10 or less flees
	EXPECT_TRUE(MoraleHolds(10, 1, 0));
	EXPECT_FALSE(MoraleHolds(9, 1, 0));
	EXPECT_TRUE(MoraleHolds(7, 0, 4)); // a guardian at its shrine +4
	EXPECT_FALSE(MoraleHolds(14, 0, -4)); // a hungry bug -4
	EXPECT_FALSE(MoraleHolds(15, 2, 0, 25)); // a dragon's terrible crit: DC 25
	// the situational modifier from the CRE's morale (10 neutral), -4..+4
	EXPECT_EQ(MoraleModifier(10), 0);
	EXPECT_EQ(MoraleModifier(12), 2);
	EXPECT_EQ(MoraleModifier(20), 4);
	EXPECT_EQ(MoraleModifier(0), -4);
}

TEST(DCC_CombatRules, GrappleTotals)
{
	// the better of Str and Agi for characters, HD for monsters
	EXPECT_EQ(GrappleModifier(true, 2, -1, 0), 2);
	EXPECT_EQ(GrappleModifier(true, -1, 1, 0), 1);
	EXPECT_EQ(GrappleModifier(false, 3, 3, 4), 4);
	// size: x2 +4, x3 +8, x4 +16, doubling on; nothing below x2
	EXPECT_EQ(GrappleSizeBonus(2, 1), 4);
	EXPECT_EQ(GrappleSizeBonus(3, 1), 8);
	EXPECT_EQ(GrappleSizeBonus(4, 1), 16);
	EXPECT_EQ(GrappleSizeBonus(5, 1), 32);
	EXPECT_EQ(GrappleSizeBonus(8, 4), 4);
	EXPECT_EQ(GrappleSizeBonus(3, 2), 0);
	EXPECT_EQ(GrappleSizeBonus(1, 1), 0);
	// the attacker has to beat the defender: a tie fails
	EXPECT_TRUE(GrappleWins(15, 14));
	EXPECT_FALSE(GrappleWins(14, 14));
	EXPECT_FALSE(GrappleWins(10, 14));
}

TEST(DCC_CombatRules, BackstabAutoCrit)
{
	// a thief of level 1+ from behind or against an unaware target, unless it has no weak points
	EXPECT_TRUE(BackstabApplies(true, 1, true, false, false));
	EXPECT_TRUE(BackstabApplies(true, 3, false, true, false));
	EXPECT_FALSE(BackstabApplies(true, 1, false, false, false));
	EXPECT_FALSE(BackstabApplies(true, 1, true, true, true)); // an ooze
	EXPECT_FALSE(BackstabApplies(false, 5, true, true, false)); // not a thief
	EXPECT_FALSE(BackstabApplies(true, 0, true, true, false)); // a 0-level peasant
	// a backstab's hit is a crit, at any total that hits; a miss is still a miss
	AttackInput in;
	in.modifier = 1;
	in.targetAC = 12;
	in.autoCrit = true;
	RollResult hit = ResolveAttack(in, 11);
	EXPECT_TRUE(hit.success);
	EXPECT_TRUE(hit.critical);
	RollResult miss = ResolveAttack(in, 10);
	EXPECT_FALSE(miss.success);
	EXPECT_FALSE(miss.critical);
	// cursed by fortune still fails
	in.cursed = true;
	EXPECT_FALSE(ResolveAttack(in, 19).critical);
}

TEST(DCC_CombatRules, Surprise)
{
	// only a creature not yet fighting, that doesn't notice the attacker
	EXPECT_TRUE(Surprised(false, false));
	EXPECT_FALSE(Surprised(false, true));
	EXPECT_FALSE(Surprised(true, false));
	EXPECT_FALSE(Surprised(true, true));
}

TEST(DCC_CombatRules, ChargeAndHigherGround)
{
	// a charge needs a run-up of half the speed
	EXPECT_TRUE(ChargeRunUp(15, 30));
	EXPECT_FALSE(ChargeRunUp(14, 30));
	EXPECT_TRUE(ChargeRunUp(10, 20));
	Situational charge = SituationalModifiers(ATK_CHARGE, false);
	EXPECT_EQ(charge.flat, CHARGE_ATTACK);
	EXPECT_EQ(SituationalModifiers(ATK_CHARGE, true).flat, 0);
	EXPECT_EQ(CHARGE_AC, -2);
	// higher ground: +1 in melee only, from two steps of the height map up
	EXPECT_TRUE(HigherGround(3, 1));
	EXPECT_FALSE(HigherGround(2, 1));
	EXPECT_FALSE(HigherGround(-7, 7));
	EXPECT_EQ(SituationalModifiers(ATK_HIGHER, false).flat, 1);
	EXPECT_EQ(SituationalModifiers(ATK_HIGHER, true).flat, 0);
	// firing into melee -1, cover -2
	EXPECT_EQ(SituationalModifiers(ATK_INTO_MELEE, true).flat, -1);
	EXPECT_EQ(SituationalModifiers(ATK_INTO_MELEE, false).flat, 0);
	EXPECT_EQ(SituationalModifiers(DEF_COVER, true).flat, -2);
	EXPECT_EQ(SituationalModifiers(DEF_COVER | ATK_INTO_MELEE, true).flat, -3);
}

TEST(DCC_CombatRules, Withdrawal)
{
	EXPECT_TRUE(WithdrawalProvokes(true, true, false, true));
	EXPECT_FALSE(WithdrawalProvokes(true, true, true, true)); // still in reach
	EXPECT_FALSE(WithdrawalProvokes(false, true, false, true)); // not engaged
	EXPECT_FALSE(WithdrawalProvokes(true, true, false, false)); // the foe can't attack
}

TEST(DCC_CombatRules, BattleRage)
{
	// a point at a time from the higher score, never below 3
	RageSpend spend = BattleRage(3, 12, 11); // ties go to Personality
	EXPECT_EQ(spend.personality, 2);
	EXPECT_EQ(spend.intelligence, 1);
	EXPECT_EQ(spend.Total(), 3);
	spend = BattleRage(5, 4, 5);
	EXPECT_EQ(spend.Total(), 3); // 4 -> 3, 5 -> 3
	EXPECT_EQ(spend.personality, 1);
	EXPECT_EQ(spend.intelligence, 2);
	EXPECT_EQ(BattleRage(0, 18, 18).Total(), 0); // the default spends nothing
	EXPECT_EQ(BattleRage(4, 3, 3).Total(), 0);
}

TEST(DCC_CombatRules, FewerActions)
{
	// doubled attacks per round: halved, at least one every other round; one action
	EXPECT_EQ(LimitedAttacks(2, ACTIONS_HALVED), 1);
	EXPECT_EQ(LimitedAttacks(4, ACTIONS_HALVED), 2);
	EXPECT_EQ(LimitedAttacks(4, ACTIONS_ONE), 2);
	EXPECT_EQ(LimitedAttacks(2, ACTIONS_ONE), 2);
	EXPECT_EQ(LimitedAttacks(6, ACTIONS_ONE | ACTIONS_HALVED), 1);
	EXPECT_EQ(LimitedAttacks(4, 0), 4);
}

TEST(DCC_CombatRules, Injuries)
{
	EXPECT_EQ(ParseInjury("HAND"), INJ_WEAPON_HAND);
	EXPECT_EQ(ParseInjury("SHIELDARM"), INJ_SHIELD_ARM);
	EXPECT_EQ(ParseInjury("EYE"), INJ_EYE);
	EXPECT_EQ(ParseInjury("nonsense"), INJ_NONE);
	EXPECT_STREQ(InjuryName(INJ_EYE), "blind in one eye");
	// the weapon hand or arm lost or useless: the other hand, -1d; no two weapons
	EXPECT_EQ(InjuryModifiers(INJ_WEAPON_HAND, false, false).dieSteps, -1);
	EXPECT_EQ(InjuryModifiers(INJ_ARM_USELESS, true, false).dieSteps, -1);
	EXPECT_TRUE(InjuryBlocksTwoWeapons(INJ_WEAPON_ARM));
	EXPECT_FALSE(InjuryBlocksTwoWeapons(INJ_EYE));
	// the shield arm: no shield AC, a two-handed weapon one-handed at -1d
	EXPECT_TRUE(InjuryBlocksShield(INJ_SHIELD_ARM));
	EXPECT_EQ(InjuryModifiers(INJ_SHIELD_ARM, false, true).dieSteps, -1);
	EXPECT_EQ(InjuryModifiers(INJ_SHIELD_ARM, false, false).dieSteps, 0);
	// one eye: -2 to missile attacks only
	EXPECT_EQ(InjuryModifiers(INJ_EYE, true, false).flat, -2);
	EXPECT_EQ(InjuryModifiers(INJ_EYE, false, false).flat, 0);
	EXPECT_EQ(InjuryModifiers(INJ_SMELL, false, false).flat, 0);
}

TEST(DCC_CombatRules, AbilityCheckSaves)
{
	// the dragon's opposed Str check is read as a DC 10 + HD Str check
	SaveSpec save;
	ASSERT_TRUE(ParseSaveSpec("STR:10:L", save));
	EXPECT_EQ(save.ability, static_cast<int>(Ability::Str));
	EXPECT_EQ(save.dc, 10);
	EXPECT_TRUE(save.plusLevel);
	ASSERT_TRUE(ParseSaveSpec("AGI:16", save));
	EXPECT_EQ(save.ability, static_cast<int>(Ability::Agi));
	EXPECT_FALSE(save.plusLevel);
	ASSERT_TRUE(ParseSaveSpec("FORT:15", save));
	EXPECT_EQ(save.ability, -1);
	EXPECT_FALSE(ParseSaveSpec("STR:A", save));
	EXPECT_FALSE(ParseSaveSpec("FOO:10", save));
}

}
}
