// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// M3.3, the end of the funnel: the gods by alignment, near deaths, how many go on

#include "ie_stats.h"

#include "../../../core/DCC/Funnel.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

static GodTable SomeGods()
{
	GodTable gods;
	// some of the Estaronan pantheon (dccgods.2da), with the DCC core gods they stand in for
	gods.Set({ 1, Alignment::Lawful, "Lom", "Ulesh", "peace, calm" });
	gods.Set({ 2, Alignment::Lawful, "Cyfawnder", "Justicia", "justice" });
	gods.Set({ 9, Alignment::Neutral, "Turil", "Aristemis", "luck" }); // neutral, though Aristemis is lawful
	gods.Set({ 13, Alignment::Chaotic, "Olyka", "Amun_Tor", "misfortune, trickery" });
	return gods;
}

TEST(DCC_Funnel, GodsByAlignment)
{
	GodTable gods = SomeGods();
	EXPECT_FALSE(gods.Empty());
	ASSERT_TRUE(gods.Find(9));
	EXPECT_EQ(gods.Find(9)->name, "Turil");
	EXPECT_EQ(gods.Find(9)->dcc, "Aristemis");
	EXPECT_EQ(gods.Find(9)->domain, "luck");
	EXPECT_EQ(gods.Find(9)->alignment, Alignment::Neutral);
	EXPECT_FALSE(gods.Find(0));
	EXPECT_FALSE(gods.Find(5));
	auto lawful = gods.For(Alignment::Lawful);
	ASSERT_EQ(lawful.size(), 2u);
	EXPECT_EQ(lawful[0]->id, 1);
	EXPECT_EQ(lawful[1]->id, 2);
	EXPECT_EQ(gods.For(Alignment::Chaotic).size(), 1u);
	EXPECT_EQ(gods.Default(Alignment::Lawful), 1);
	EXPECT_EQ(gods.Default(Alignment::Chaotic), 13);
	EXPECT_EQ(GodTable().Default(Alignment::Neutral), 0);
	// a later Set with the same id replaces it
	gods.Set({ 2, Alignment::Lawful, "Cogada", "Klazath", "war" });
	EXPECT_EQ(gods.Find(2)->name, "Cogada");
	EXPECT_EQ(gods.For(Alignment::Lawful).size(), 2u);
}

TEST(DCC_Funnel, TheGodFollowsTheAlignment)
{
	// the player changes a level-0 character's alignment: a god of that alignment stays, another goes
	GodTable gods = SomeGods();
	EXPECT_EQ(gods.Follow(1, Alignment::Lawful), 1);
	EXPECT_EQ(gods.Follow(2, Alignment::Lawful), 2);
	EXPECT_EQ(gods.Follow(1, Alignment::Chaotic), 0);
	EXPECT_EQ(gods.Follow(13, Alignment::Neutral), 0);
	EXPECT_EQ(gods.Follow(0, Alignment::Lawful), 0);
	EXPECT_EQ(gods.Follow(77, Alignment::Lawful), 0);
}

TEST(DCC_Funnel, NearDeaths)
{
	// below 2 hp from 2 or more, and still standing
	EXPECT_TRUE(NearDeath(4, 1));
	EXPECT_TRUE(NearDeath(2, 1));
	EXPECT_FALSE(NearDeath(1, 1)); // already there
	EXPECT_FALSE(NearDeath(3, 2));
	EXPECT_FALSE(NearDeath(4, 0)); // dead (or bleeding: counted when the bleeding starts)
	EXPECT_FALSE(NearDeath(1, 3)); // healed
	EXPECT_FALSE(NearDeath(0, 1)); // rolled over: counted there
}

TEST(DCC_Funnel, HowManyGoOn)
{
	EXPECT_EQ(FUNNEL_PARTY_SIZE, 4);
	EXPECT_EQ(FunnelPicks(16), 4);
	EXPECT_EQ(FunnelPicks(4), 4);
	EXPECT_EQ(FunnelPicks(3), 3);
	EXPECT_EQ(FunnelPicks(1), 1);
	EXPECT_EQ(FunnelPicks(0), 0);
	EXPECT_EQ(FunnelPicks(-2), 0);
}

TEST(DCC_Funnel, AlignmentCodes)
{
	// FunnelRoll's codes (1 lawful, 2 neutral, 3 chaotic) and IE_ALIGNMENT
	EXPECT_EQ(AlignmentCodeToIE(1), 0x12u);
	EXPECT_EQ(AlignmentCodeToIE(2), 0x22u);
	EXPECT_EQ(AlignmentCodeToIE(3), 0x32u);
	EXPECT_EQ(AlignmentCodeToIE(0), 0u);
	EXPECT_EQ(AlignmentCodeToIE(4), 0u);
	for (int code = 1; code <= 3; ++code) {
		EXPECT_EQ(AlignmentCode(AlignmentCodeToIE(code)), code);
	}
	EXPECT_EQ(AlignmentCode(0x11), 1); // lawful good
	EXPECT_EQ(AlignmentCode(0x33), 3); // chaotic evil
	EXPECT_EQ(AlignmentFromIE(AlignmentCodeToIE(3)), Alignment::Chaotic);
	EXPECT_STREQ(RESERVE_AREA, "DCCRESV");
}

}
}
