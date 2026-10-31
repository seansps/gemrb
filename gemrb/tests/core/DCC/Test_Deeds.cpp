// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Mighty Deeds of Arms: when a deed succeeds, its strength by deed die result,
// which deed an attack declares (one-off, signature, weapon) and the deed table.
// docs/rules/combat.md, classes.md, open-questions.md

#include "../../../core/DCC/Deeds.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

TEST(DCC_Deeds, SuccessNeedsAHitAndThree)
{
	EXPECT_FALSE(DeedSucceeds(1, true));
	EXPECT_FALSE(DeedSucceeds(2, true));
	EXPECT_TRUE(DeedSucceeds(3, true));
	EXPECT_TRUE(DeedSucceeds(10, true));
	// a miss is never a deed
	EXPECT_FALSE(DeedSucceeds(3, false));
	EXPECT_FALSE(DeedSucceeds(7, false));
}

TEST(DCC_Deeds, StrengthByResult)
{
	EXPECT_EQ(DeedStrength(1), 0);
	EXPECT_EQ(DeedStrength(2), 0);
	for (int roll = 3; roll <= 7; ++roll) {
		EXPECT_EQ(DeedStrength(roll), roll);
	}
	// 7 and up use the 7+ row (a d10 deed die)
	EXPECT_EQ(DeedStrength(8), 7);
	EXPECT_EQ(DeedStrength(10), 7);
}

TEST(DCC_Deeds, OneOffThenSignatureThenWeapon)
{
	EXPECT_EQ(ChooseDeed(Deed::Blind, Deed::Trip, Deed::Push), Deed::Blind);
	EXPECT_EQ(ChooseDeed(Deed::None, Deed::Trip, Deed::Push), Deed::Trip);
	EXPECT_EQ(ChooseDeed(Deed::None, Deed::None, Deed::Push), Deed::Push);
	EXPECT_EQ(ChooseDeed(Deed::None, Deed::None, Deed::None), Deed::None);
	EXPECT_EQ(ChooseDeed(Deed::Disarm, Deed::None, Deed::None), Deed::Disarm);
	// flails trip, hammers push, the rest have nothing of their own
	EXPECT_EQ(WeaponDeed(23), Deed::Trip);
	EXPECT_EQ(WeaponDeed(21), Deed::Push);
	EXPECT_EQ(WeaponDeed(20), Deed::None);
	EXPECT_EQ(DEFAULT_SIGNATURE_DEED, Deed::Trip);
}

TEST(DCC_Deeds, Keys)
{
	for (int i = 1; i < DEED_COUNT; ++i) {
		Deed deed = static_cast<Deed>(i);
		EXPECT_EQ(ParseDeedKey(DeedKey(deed)), deed);
	}
	EXPECT_EQ(ParseDeedKey("trip"), Deed::Trip);
	EXPECT_EQ(ParseDeedKey("SUNDER"), Deed::None);
	EXPECT_STREQ(DeedKey(Deed::None), "");
}

static std::vector<ResultRow> Rows(int from, int to)
{
	std::vector<ResultRow> rows;
	for (int roll = from; roll <= to; ++roll) {
		ResultRow row;
		row.roll = roll;
		row.text = std::to_string(roll);
		rows.push_back(row);
	}
	return rows;
}

TEST(DCC_Deeds, Table)
{
	DeedTable table;
	EXPECT_TRUE(table.IsEmpty());
	ASSERT_TRUE(table.SetRows(Deed::Trip, Rows(3, 7)));
	EXPECT_FALSE(table.IsEmpty());
	EXPECT_EQ(table.Lookup(Deed::Trip, 2), nullptr);
	ASSERT_NE(table.Lookup(Deed::Trip, 3), nullptr);
	EXPECT_EQ(table.Lookup(Deed::Trip, 3)->text, "3");
	EXPECT_EQ(table.Lookup(Deed::Trip, 6)->text, "6");
	EXPECT_EQ(table.Lookup(Deed::Trip, 9)->text, "7");
	EXPECT_EQ(table.Lookup(Deed::None, 5), nullptr);
	EXPECT_EQ(table.Lookup(Deed::Blind, 5), nullptr);
	// a category must have rows 3..7
	EXPECT_FALSE(table.SetRows(Deed::Blind, Rows(3, 6)));
	EXPECT_FALSE(table.SetRows(Deed::Blind, Rows(2, 7)));
	EXPECT_FALSE(table.SetRows(Deed::None, Rows(3, 7)));
}

TEST(DCC_Deeds, SaveAgainstTheAttackRoll)
{
	SaveSpec save;
	ASSERT_TRUE(ParseSaveSpec("REF:A", save));
	EXPECT_TRUE(save.vsAttack);
	EXPECT_FALSE(save.IsNone());
	EXPECT_EQ(save.save, Save::Ref);
	ASSERT_TRUE(ParseSaveSpec("FORT:12", save));
	EXPECT_FALSE(save.vsAttack);
	EXPECT_FALSE(ParseSaveSpec("REF:A:L", save));
}

}
}
