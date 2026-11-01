// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// M2.11: how DCC characters look: the hair and beard overlays, the overlays' colour bytes, the stances

#include "../../../core/CharAnimations.h"
#include "../../../core/DCC/Combat.h"
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

// the "held two-handed" switch: the staff, spear, pitchfork, shovel and pole are one-handed in DCC
// but held in both hands, so they take the two-handed stances while the off hand is free
TEST(DCC_Looks, HeldInBothHands)
{
	HeldTable table;
	EXPECT_TRUE(table.Empty());
	table.Set("QS", true); // staff, pole
	table.Set("sp", true); // spear (keys are upper case)
	table.Set("PF", true);
	table.Set("SV", true);
	table.Set("FNTSTAF", false); // an item taken out of its code's row
	EXPECT_FALSE(table.Empty());
	EXPECT_TRUE(table.BothHands("", "QS"));
	EXPECT_TRUE(table.BothHands("dccspear", "SP"));
	EXPECT_TRUE(table.BothHands("x", "pf"));
	EXPECT_FALSE(table.BothHands("fntstaf", "QS")); // the item's row wins
	EXPECT_FALSE(table.BothHands("dccclub", "CL")); // not listed
	EXPECT_FALSE(table.BothHands("", "")); // no animation code
	EXPECT_FALSE(table.BothHands("", "S2")); // the true two-handed weapons go by their flag, not the table
}

TEST(DCC_Looks, HeldWeaponStance)
{
	// a weapon held in both hands: two-handed with the off hand free, one-handed with a shield
	EXPECT_EQ(WeaponAnimType(false, true, OffHand::Empty), IE_ANI_WEAPON_2H);
	EXPECT_EQ(WeaponAnimType(false, true, OffHand::Shield), IE_ANI_WEAPON_1H);
	EXPECT_EQ(WeaponAnimType(false, true, OffHand::Weapon), IE_ANI_WEAPON_2W);
	// an ordinary one-handed weapon as stock GemRB has it
	EXPECT_EQ(WeaponAnimType(false, false, OffHand::Empty), IE_ANI_WEAPON_1H);
	EXPECT_EQ(WeaponAnimType(false, false, OffHand::Shield), IE_ANI_WEAPON_1H);
	EXPECT_EQ(WeaponAnimType(false, false, OffHand::Weapon), IE_ANI_WEAPON_2W);
	// the true two-handed weapons (the ITM's flag) stay two-handed whatever the table says
	EXPECT_EQ(WeaponAnimType(true, false, OffHand::Empty), IE_ANI_WEAPON_2H);
	EXPECT_EQ(WeaponAnimType(true, true, OffHand::Empty), IE_ANI_WEAPON_2H);
}

TEST(DCC_Looks, HeldWeaponRulesUnchanged)
{
	// the rules read the ITM's two-handed flag only: a spear held in both hands still rolls d20
	// initiative and may take a shield; the true two-handed weapons roll d16 (equipment.md)
	bool spearFlag = false; // DCC one-handed: the ITM has no two-handed flag, whatever its stance
	bool polearmFlag = true;
	EXPECT_EQ(WeaponAnimType(spearFlag, true, OffHand::Empty), IE_ANI_WEAPON_2H); // the look
	EXPECT_EQ(InitiativeDie(spearFlag), 20); // the rule
	EXPECT_EQ(InitiativeDie(polearmFlag), 16);
}

}
}
