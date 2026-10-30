// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "../../../core/DCC/AbilityMod.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

// docs/rules/abilities.md, Table 1-1: score, modifier, wizard spells known, max spell level
static const std::vector<AbilityModRow> table11 {
	{ 3, -3, 0, 0 },
	{ 4, -2, -2, 1 },
	{ 5, -2, -2, 1 },
	{ 6, -1, -1, 1 },
	{ 7, -1, -1, 1 },
	{ 8, -1, 0, 2 },
	{ 9, 0, 0, 2 },
	{ 10, 0, 0, 3 },
	{ 11, 0, 0, 3 },
	{ 12, 0, 0, 4 },
	{ 13, 1, 0, 4 },
	{ 14, 1, 1, 4 },
	{ 15, 1, 1, 5 },
	{ 16, 2, 1, 5 },
	{ 17, 2, 2, 5 },
	{ 18, 3, 2, 5 }
};

// the curve as the spec sums it up: 3, 4-5, 6-8, 9-12, 13-15, 16-17, 18
static int Curve(int score)
{
	if (score <= 3) return -3;
	if (score <= 5) return -2;
	if (score <= 8) return -1;
	if (score <= 12) return 0;
	if (score <= 15) return 1;
	if (score <= 17) return 2;
	return 3;
}

static AbilityModTable Table11()
{
	AbilityModTable table;
	EXPECT_TRUE(table.SetRows(table11));
	return table;
}

TEST(DCC_AbilityMod, EveryScore)
{
	AbilityModTable table = Table11();
	EXPECT_EQ(table.MinScore(), 3);
	EXPECT_EQ(table.MaxScore(), 18);
	for (const auto& row : table11) {
		EXPECT_EQ(table.Modifier(row.score), row.mod) << "score " << row.score;
		EXPECT_EQ(table.Modifier(row.score), Curve(row.score)) << "score " << row.score;
		EXPECT_EQ(table.WizardSpells(row.score), row.wizardSpells) << "score " << row.score;
		EXPECT_EQ(table.MaxSpellLevel(row.score), row.maxSpellLevel) << "score " << row.score;
		EXPECT_EQ(table.Get(AbilityModTable::Column::Modifier, row.score), row.mod);
	}
}

TEST(DCC_AbilityMod, Spellcasting)
{
	AbilityModTable table = Table11();
	EXPECT_FALSE(table.CanCast(3));
	for (int score = 4; score <= 18; ++score) {
		EXPECT_TRUE(table.CanCast(score)) << "score " << score;
	}
}

TEST(DCC_AbilityMod, OutOfRangeClamps)
{
	// the spec leaves scores outside 3-18 open; we clamp to the table
	AbilityModTable table = Table11();
	for (int score : { 2, 1, 0, -5 }) {
		EXPECT_EQ(table.Modifier(score), -3) << "score " << score;
		EXPECT_EQ(table.MaxSpellLevel(score), 0) << "score " << score;
		EXPECT_FALSE(table.CanCast(score));
	}
	for (int score : { 19, 20, 25, 40 }) {
		EXPECT_EQ(table.Modifier(score), 3) << "score " << score;
		EXPECT_EQ(table.WizardSpells(score), 2) << "score " << score;
		EXPECT_EQ(table.MaxSpellLevel(score), 5) << "score " << score;
	}
}

TEST(DCC_AbilityMod, RowOrderDoesNotMatter)
{
	std::vector<AbilityModRow> reversed(table11.rbegin(), table11.rend());
	AbilityModTable table;
	ASSERT_TRUE(table.SetRows(reversed));
	for (int score = 3; score <= 18; ++score) {
		EXPECT_EQ(table.Modifier(score), Curve(score));
	}
}

TEST(DCC_AbilityMod, BadRowsLeaveTableEmpty)
{
	AbilityModTable empty;
	EXPECT_TRUE(empty.IsEmpty());
	EXPECT_EQ(empty.Modifier(18), 0);
	EXPECT_EQ(empty.Modifier(3), 0);

	AbilityModTable table;
	EXPECT_FALSE(table.SetRows({}));
	EXPECT_TRUE(table.IsEmpty());

	auto gap = table11;
	gap.erase(gap.begin() + 5);
	EXPECT_FALSE(table.SetRows(gap));
	EXPECT_TRUE(table.IsEmpty());

	auto duplicate = table11;
	duplicate.push_back(table11[4]);
	EXPECT_FALSE(table.SetRows(duplicate));
	EXPECT_TRUE(table.IsEmpty());
}

}
}
