// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// M2.11: how DCC characters look: the hair and beard overlays, the overlays' colour bytes, the stances

#include "../../../core/DCC/Looks.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

TEST(DCC_Looks, HairStyles)
{
	HairTable table;
	EXPECT_TRUE(table.Empty());
	table.Set({ 1, false, "HP", "" });
	table.Set({ 2, false, "HL", "HC" }); // a long style, cropped under a helmet
	table.Set({ 1, true, "BD", "BD" });
	EXPECT_FALSE(table.Empty());
	// hair and beards have ids of their own
	ASSERT_NE(table.Find(1, false), nullptr);
	ASSERT_NE(table.Find(1, true), nullptr);
	EXPECT_EQ(table.Find(1, true)->code, "BD");
	EXPECT_EQ(table.Find(2, true), nullptr);
	EXPECT_EQ(table.Find(0, false), nullptr);
	EXPECT_EQ(table.Find(-1, false), nullptr);
	// what is drawn, without and with a helmet
	EXPECT_EQ(HairOverlay(table.Find(1, false), false), "HP");
	EXPECT_EQ(HairOverlay(table.Find(1, false), true), "");
	EXPECT_EQ(HairOverlay(table.Find(2, false), true), "HC");
	EXPECT_EQ(HairOverlay(table.Find(1, true), true), "BD");
	EXPECT_EQ(HairOverlay(nullptr, false), "");
	// a row again replaces it
	table.Set({ 1, false, "HS", "" });
	EXPECT_EQ(HairOverlay(table.Find(1, false), false), "HS");
}

TEST(DCC_Looks, OverlayColourBytes)
{
	// the body's parts (MAIN..MAIN_5) take byte 0; weapon, off-hand and helmet bytes 1, 2, 3
	for (unsigned int type = 0; type < 5; ++type) {
		EXPECT_EQ(OverlayColourByte(type), 0U) << type;
	}
	EXPECT_EQ(OverlayColourByte(5), 1U);
	EXPECT_EQ(OverlayColourByte(6), 2U);
	EXPECT_EQ(OverlayColourByte(7), 3U);
	EXPECT_EQ(OverlayColourByte(8), 0U);
}

TEST(DCC_Looks, Stances)
{
	EXPECT_FALSE(CombatStance(false, false)); // awake, the relaxed idle
	EXPECT_TRUE(CombatStance(true, false)); // ready while engaged
	EXPECT_TRUE(CombatStance(false, true)); // and while the party fights
	EXPECT_TRUE(CombatStance(true, true));
}

}
}
