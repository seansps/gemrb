// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC combat table logic: dice and duration parsing, the crit/fumble effect
// grammar, clamped lookups, monster crit entries and the two-weapon dice
// (Table 4-3, with the halfling exception). The shipped 2DAs are checked
// against the spec in tests/2DAImporter/Test_DCCTables.cpp.

#include "../../../core/DCC/CombatTables.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

TEST(DCC_CombatTables, ParseDice)
{
	DiceExpr dice;
	ASSERT_TRUE(ParseDice("2d6", dice));
	EXPECT_EQ(dice.count, 2);
	EXPECT_EQ(dice.sides, 6);
	EXPECT_EQ(dice.add, 0);
	ASSERT_TRUE(ParseDice("1d10+5", dice));
	EXPECT_EQ(dice.add, 5);
	ASSERT_TRUE(ParseDice("d4", dice));
	EXPECT_EQ(dice.count, 1);
	ASSERT_TRUE(ParseDice("1d4-1", dice));
	EXPECT_EQ(dice.add, -1);
	ASSERT_TRUE(ParseDice("3", dice));
	EXPECT_EQ(dice.count, 0);
	EXPECT_EQ(dice.add, 3);
	ASSERT_TRUE(ParseDice("*", dice));
	EXPECT_TRUE(dice.IsNone());
	EXPECT_FALSE(ParseDice("2x6", dice));
	EXPECT_FALSE(ParseDice("d", dice));
	EXPECT_EQ(DiceString(DiceExpr { 2, 4, 1 }), "2d4+1");
	EXPECT_EQ(DiceString(DiceExpr { 1, 8, 0 }), "1d8");

	int calls = 0;
	EXPECT_EQ(RollDice(DiceExpr { 3, 6, 2 }, [&calls](int sides) {
			  ++calls;
			  return sides;
		  }),
		  20);
	EXPECT_EQ(calls, 3);
}

TEST(DCC_CombatTables, ParseDuration)
{
	Duration duration;
	ASSERT_TRUE(ParseDuration("1d4", duration));
	EXPECT_EQ(duration.kind, Duration::Kind::Rounds);
	EXPECT_EQ(duration.dice.sides, 4);
	ASSERT_TRUE(ParseDuration("2", duration));
	EXPECT_EQ(duration.dice.add, 2);
	ASSERT_TRUE(ParseDuration("1d6D", duration));
	EXPECT_EQ(duration.kind, Duration::Kind::Days);
	ASSERT_TRUE(ParseDuration("1d5H", duration));
	EXPECT_EQ(duration.kind, Duration::Kind::Hours);
	ASSERT_TRUE(ParseDuration("1T", duration));
	EXPECT_EQ(duration.kind, Duration::Kind::Turns);
	ASSERT_TRUE(ParseDuration("1d4W", duration));
	EXPECT_EQ(duration.kind, Duration::Kind::Weeks);
	ASSERT_TRUE(ParseDuration("PERM", duration));
	EXPECT_EQ(duration.kind, Duration::Kind::Permanent);
	ASSERT_TRUE(ParseDuration("HEAL", duration));
	EXPECT_EQ(duration.kind, Duration::Kind::UntilHealed);
	ASSERT_TRUE(ParseDuration("", duration));
	EXPECT_EQ(duration.dice.add, 1);
	EXPECT_FALSE(ParseDuration("xyz", duration));
	EXPECT_EQ(DurationSeconds(Duration::Kind::Rounds, 3, 6), 18);
	EXPECT_EQ(DurationSeconds(Duration::Kind::Turns, 1, 6), 600);
	EXPECT_EQ(DurationSeconds(Duration::Kind::Hours, 2, 6), 7200);
}

TEST(DCC_CombatTables, ParseSaveSpec)
{
	SaveSpec save;
	ASSERT_TRUE(ParseSaveSpec("FORT:15:L", save));
	EXPECT_EQ(save.save, Save::Fort);
	EXPECT_EQ(save.dc, 15);
	EXPECT_TRUE(save.plusLevel);
	ASSERT_TRUE(ParseSaveSpec("REF:10", save));
	EXPECT_EQ(save.save, Save::Ref);
	EXPECT_FALSE(save.plusLevel);
	ASSERT_TRUE(ParseSaveSpec("*", save));
	EXPECT_TRUE(save.IsNone());
	EXPECT_FALSE(ParseSaveSpec("LUCK:10", save));
	EXPECT_FALSE(ParseSaveSpec("FORT:x", save));
	EXPECT_FALSE(ParseSaveSpec("FORT:10:Q", save));
}

TEST(DCC_CombatTables, ParseEffects)
{
	std::vector<TableAlternatives> effects;
	ASSERT_TRUE(ParseEffects("*", effects));
	EXPECT_TRUE(effects.empty());
	ASSERT_TRUE(ParseEffects("PRONE:1+EXTRA", effects));
	ASSERT_EQ(effects.size(), 2u);
	EXPECT_EQ(effects[0][0].key, "PRONE");
	EXPECT_EQ(effects[0][0].args, std::vector<std::string> { "1" });
	EXPECT_EQ(effects[1][0].key, "EXTRA");
	ASSERT_TRUE(ParseEffects("SHIELD|?UNCON:1d6+ABIL:INT:1d4:PERM@NOHELM+!DMG:2d6", effects));
	ASSERT_EQ(effects.size(), 3u);
	ASSERT_EQ(effects[0].size(), 2u);
	EXPECT_EQ(effects[0][0].key, "SHIELD");
	EXPECT_EQ(effects[0][1].gate, '?');
	EXPECT_EQ(effects[0][1].key, "UNCON");
	EXPECT_EQ(effects[1][0].condition, "NOHELM");
	EXPECT_EQ(effects[1][0].args.size(), 3u);
	EXPECT_EQ(effects[2][0].gate, '!');
	EXPECT_FALSE(ParseEffects("PRONE+", effects));
	EXPECT_FALSE(ParseEffects("A||B", effects));
}

static ResultRow Row(int roll, const std::string& text)
{
	ResultRow row;
	row.roll = roll;
	row.text = text;
	return row;
}

TEST(DCC_CombatTables, ResultTableClamps)
{
	ResultTable table;
	EXPECT_FALSE(table.SetRows({ Row(0, "a"), Row(2, "c") })); // a gap
	ASSERT_TRUE(table.SetRows({ Row(1, "b"), Row(0, "a"), Row(2, "c") }));
	EXPECT_EQ(table.MinRoll(), 0);
	EXPECT_EQ(table.MaxRoll(), 2);
	EXPECT_EQ(table.Lookup(-3)->text, "a"); // anything lower is the first row
	EXPECT_EQ(table.Lookup(1)->text, "b");
	EXPECT_EQ(table.Lookup(9)->text, "c"); // anything higher the last
}

TEST(DCC_CombatTables, CritTableNames)
{
	EXPECT_STREQ(CritTableName(CritTableID::III), "III");
	EXPECT_EQ(ParseCritTableName("V"), CritTableID::V);
	EXPECT_EQ(ParseCritTableName("m"), CritTableID::M);
	EXPECT_EQ(ParseCritTableName("DR"), CritTableID::DR);
	EXPECT_EQ(ParseCritTableName("VI"), CritTableID::None);
}

TEST(DCC_CombatTables, MonsterCrits)
{
	MonsterCrit crit;
	ASSERT_TRUE(ParseMonsterCrit("III/d8", crit));
	EXPECT_EQ(crit.table, CritTableID::III);
	EXPECT_EQ(crit.dice, 1);
	EXPECT_EQ(crit.die, 8);
	ASSERT_TRUE(ParseMonsterCrit("V/2d10", crit));
	EXPECT_EQ(crit.dice, 2);
	ASSERT_TRUE(ParseMonsterCrit("-", crit));
	EXPECT_EQ(crit.table, CritTableID::None);
	EXPECT_FALSE(ParseMonsterCrit("X/d4", crit));
	EXPECT_FALSE(ParseMonsterCrit("M", crit));

	MonsterCritTable table;
	MonsterCritTable::Row row0;
	row0[static_cast<size_t>(MonsterType::Other)] = MonsterCrit { CritTableID::M, 1, 4 };
	MonsterCritTable::Row row1;
	row1[static_cast<size_t>(MonsterType::Other)] = MonsterCrit { CritTableID::M, 1, 6 };
	ASSERT_TRUE(table.SetRows({ row0, row1 }));
	EXPECT_EQ(table.Lookup(0, MonsterType::Other).die, 4);
	EXPECT_EQ(table.Lookup(7, MonsterType::Other).die, 6); // clamps to the last row
}

static TwoWeaponTable Table43()
{
	TwoWeaponTable table;
	table.SetRows({ { 8, -3, -4, CritRule::None }, { 11, -2, -3, CritRule::None }, { 15, -1, -2, CritRule::None }, { 17, -1, -1, CritRule::MaxOnHit }, { 99, 0, -1, CritRule::Normal } });
	return table;
}

TEST(DCC_CombatTables, TwoWeaponDiceByAgility)
{
	TwoWeaponTable table = Table43();
	ASSERT_FALSE(table.IsEmpty());
	// with a d20 action die: <=8 d12/d10, 9-11 d14/d12, 12-15 d16/d14, 16-17 d16/d16, 18+ d20/d16
	struct Case {
		int agility;
		int primary;
		int off;
		CritRule crit;
	};
	const Case cases[] = { { 3, 12, 10, CritRule::None }, { 8, 12, 10, CritRule::None }, { 9, 14, 12, CritRule::None }, { 11, 14, 12, CritRule::None },
			       { 12, 16, 14, CritRule::None }, { 15, 16, 14, CritRule::None }, { 16, 16, 16, CritRule::MaxOnHit }, { 17, 16, 16, CritRule::MaxOnHit },
			       { 18, 20, 16, CritRule::Normal } };
	for (const auto& c : cases) {
		TwoWeaponDice dice = table.Lookup(c.agility, false);
		EXPECT_EQ(AttackDie(20, dice.primarySteps), c.primary) << "Agi " << c.agility;
		EXPECT_EQ(AttackDie(20, dice.offSteps), c.off) << "Agi " << c.agility;
		EXPECT_EQ(dice.primaryCrit, c.crit) << "Agi " << c.agility;
		EXPECT_EQ(dice.offCrit, CritRule::None) << "Agi " << c.agility;
	}
	// stacks with a smaller action die: d16 -1d is d14
	EXPECT_EQ(AttackDie(16, table.Lookup(16, false).primarySteps), 14);
}

TEST(DCC_CombatTables, TwoWeaponHalfling)
{
	TwoWeaponTable table = Table43();
	// at least Agi 16: d16/d16, and a natural 16 on either hand auto-hits and crits
	TwoWeaponDice dice = table.Lookup(9, true);
	EXPECT_EQ(AttackDie(20, dice.primarySteps), 16);
	EXPECT_EQ(AttackDie(20, dice.offSteps), 16);
	EXPECT_EQ(dice.primaryCrit, CritRule::MaxAutoHit);
	EXPECT_EQ(dice.offCrit, CritRule::MaxAutoHit);
	// Agi 18 uses the normal row (d20 primary crits as normal)
	dice = table.Lookup(18, true);
	EXPECT_EQ(AttackDie(20, dice.primarySteps), 20);
	EXPECT_EQ(dice.primaryCrit, CritRule::Normal);
}

TEST(DCC_CombatTables, ArmorFumbleDice)
{
	ArmorFumbleTable table;
	table.Set("ANIM_2A", 8);
	table.Set("ANIM_3A", 12);
	table.Set("SHIELD", 8);
	table.Set("DCTPLATE", 16);
	EXPECT_EQ(table.ArmorDie("whatever", "2A"), 8);
	EXPECT_EQ(table.ArmorDie("dctplate", "3A"), 16); // the resref wins
	EXPECT_EQ(table.ArmorDie("robe", "1A"), 0); // unknown: no armor
	EXPECT_EQ(table.ShieldDie("buckler"), 8);
	EXPECT_EQ(FumbleDie(table.ArmorDie("x", "3A"), table.ShieldDie("y")), 12);
}

}
}
