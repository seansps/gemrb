// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// M2.10: encumbrance by body weight (equipment.md, an Ethren decision) and the party AI record

#include "../../../core/DCC/Body.h"
#include "../../../core/DCC/Extension.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

static BodyTable Table()
{
	BodyTable table;
	table.Set({ 1, 110, 5, 10, 33, 50 });
	table.Set({ 2, 80, 4, 10, 33, 50 });
	table.Set({ 4, 120, 4, 10, 33, 50 });
	table.Set({ 5, 40, 3, 10, 33, 50 });
	return table;
}

TEST(DCC_Body, WeightByRace)
{
	BodyTable table = Table();
	EXPECT_EQ(table.Average(1), 137); // 110 + 5 x 5.5
	EXPECT_EQ(table.Average(2), 102);
	EXPECT_EQ(table.Average(4), 142);
	EXPECT_EQ(table.Average(5), 56);
	// an unknown race is human
	EXPECT_EQ(table.Average(7), 137);
	EXPECT_EQ(table.Row(7).race, 1);
	// the dice: lowest, highest, and a die out of range is clamped
	EXPECT_EQ(table.Roll(1, [](int) { return 1; }), 115);
	EXPECT_EQ(table.Roll(1, [](int sides) { return sides; }), 160);
	EXPECT_EQ(table.Roll(5, [](int sides) { return sides; }), 70); // the book: no halfling weighs more than 70
	EXPECT_EQ(table.Roll(5, [](int) { return 99; }), 70);
	int calls = 0;
	EXPECT_EQ(table.Roll(4, [&calls](int sides) {
		++calls;
		return sides == 10 ? 3 : 0;
	}),
		  132);
	EXPECT_EQ(calls, 4);
	// no table loaded: the human row stands in
	EXPECT_EQ(BodyTable().Average(5), 137);
}

TEST(DCC_Body, LoadLimits)
{
	BodyTable table = Table();
	LoadLimits human = LimitsFor(140, table.Row(1));
	EXPECT_EQ(human.half, 46); // a third, rounded down
	EXPECT_EQ(human.cap, 70); // half the body weight
	EXPECT_EQ(LoadFor(0, human), Load::Light);
	EXPECT_EQ(LoadFor(46, human), Load::Light);
	EXPECT_EQ(LoadFor(47, human), Load::Heavy);
	EXPECT_EQ(LoadFor(70, human), Load::Heavy);
	EXPECT_EQ(LoadFor(71, human), Load::Over);
	EXPECT_EQ(SpeedDivisor(Load::Light), 1);
	EXPECT_EQ(SpeedDivisor(Load::Heavy), 2);
	EXPECT_EQ(SpeedDivisor(Load::Over), 0);
	// a halfling of 56 lb: 18 and 28
	LoadLimits halfling = LimitsFor(56, table.Row(5));
	EXPECT_EQ(halfling.half, 18);
	EXPECT_EQ(halfling.cap, 28);
	// a body weight of 0 never divides by nothing
	LoadLimits none = LimitsFor(0, table.Row(1));
	EXPECT_EQ(LoadFor(0, none), Load::Light);
	EXPECT_EQ(LoadFor(1, none), Load::Over);
	EXPECT_STREQ(LoadName(Load::Heavy), "heavy");
}

TEST(DCC_Body, CharacterRecord)
{
	CharacterExt ext;
	EXPECT_TRUE(ext.Empty());
	ext.body.weight = 142;
	EXPECT_FALSE(ext.Empty());
	ext.body.partyAI = PartyAI::Flee;
	std::vector<uint8_t> bytes = WriteExtension(ext);
	// header and CHAR (12 + 4)
	EXPECT_EQ(bytes.size(), EXT_HEADER_SIZE + 16);
	CharacterExt back;
	ASSERT_TRUE(ReadExtension(bytes.data(), bytes.size(), back));
	EXPECT_EQ(back.body.weight, 142);
	EXPECT_EQ(back.body.partyAI, PartyAI::Flee);
	EXPECT_TRUE(back.other.empty());
	// the mode alone is worth a record; a mode past the known ones reads as the last
	CharacterExt passive;
	passive.body.partyAI = PartyAI::Passive;
	EXPECT_FALSE(passive.Empty());
	bytes = WriteExtension(passive);
	bytes[EXT_HEADER_SIZE + EXT_RECORD_HEADER_SIZE + 2] = 9;
	ASSERT_TRUE(ReadExtension(bytes.data(), bytes.size(), back));
	EXPECT_EQ(back.body.partyAI, PartyAI::Engage);
	EXPECT_EQ(back.body.weight, 0);
	EXPECT_STREQ(PartyAIName(PartyAI::Defend), "defend");
	EXPECT_STREQ(PARTY_AI_SCRIPT, "DCCPARTY");
}

}
}
