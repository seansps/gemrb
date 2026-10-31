// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// learning spells by level (core/DCC/Learning.h): spells known by count, Table 5-8's random
// picks with substitutes for rows without content, the cleric's default picks

#include "../../../core/DCC/Learning.h"

#include <gtest/gtest.h>

#include <deque>

namespace GemRB {
namespace DCC {

// a level-1 Table 5-8 with the starter set's content on rows 4 (dwmmis), 5 (dwslee), a cleric
// spell on 2, the patron pair on 3 and 6, the patron's spell on 7
static SpellLists TestLists()
{
	SpellLists lists;
	std::vector<SpellListEntry> wizard = {
		{ 1, 1, "Chill touch", "", LIST_NONE },
		{ 2, 1, "Detect magic", "dcdete", LIST_CLERIC_SPELL },
		{ 3, 1, "Invoke patron", "", LIST_PATRON_PAIR },
		{ 4, 1, "Magic missile", "dwmmis", LIST_NONE },
		{ 5, 1, "Sleep", "dwslee", LIST_NONE },
		{ 6, 1, "Patron bond", "", LIST_PATRON_PAIR },
		{ 7, 1, "(Patron spell)", "", LIST_PATRON_SPELL },
	};
	lists.Set(CasterKind::Wizard, 1, wizard);
	std::vector<SpellListEntry> cleric = {
		{ 1, 1, "Blessing", "dcbles", LIST_NONE },
		{ 2, 1, "Darkness", "", LIST_NONE },
		{ 3, 1, "Paralysis", "dcpara", LIST_NONE },
	};
	lists.Set(CasterKind::Cleric, 1, cleric);
	return lists;
}

// dice in order, then 1s
static Roller Dice(std::deque<int> faces)
{
	auto queue = std::make_shared<std::deque<int>>(std::move(faces));
	return [queue](int sides) {
		if (queue->empty()) return 1;
		int face = queue->front();
		queue->pop_front();
		return std::min(std::max(face, 1), sides);
	};
}

static bool Wizardly(const std::string& spell)
{
	return spell == "dwmmis" || spell == "dwslee";
}

TEST(DCC_Learning, SpellsKnownByCount)
{
	// Table 1-12 level 1 is 4; Table 1-1 Int 17-18 +2, 14-16 +1, 4-5 -2 (at least 1), 3 no casting
	EXPECT_EQ(ArcaneSpellsKnown(4, 0, true), 4);
	EXPECT_EQ(ArcaneSpellsKnown(4, 2, true), 6);
	EXPECT_EQ(ArcaneSpellsKnown(3, -2, true), 1); // an elf at level 1 with Int 4 or 5
	EXPECT_EQ(ArcaneSpellsKnown(4, -2, false), 0);
	EXPECT_EQ(ArcaneSpellsKnown(0, 2, true), 0); // not a class with spells known
	EXPECT_EQ(ReplaceableSpells(4), 2);
	EXPECT_EQ(ReplaceableSpells(3), 1);
	EXPECT_EQ(ReplaceableSpells(1), 0);

	ClericSpellsTable cleric;
	cleric.Set(1, { 4, 0, 0, 0, 0 });
	cleric.Set(3, { 5, 3, 0, 0, 0 });
	EXPECT_EQ(cleric.Known(1, 1), 4);
	EXPECT_EQ(cleric.Known(1, 2), 0);
	EXPECT_EQ(cleric.Known(3, 2), 3);
	EXPECT_EQ(cleric.Known(11, 1), 0);
	EXPECT_EQ(cleric.Known(1, 6), 0);
}

TEST(DCC_Learning, RandomPicksWithContent)
{
	SpellLists lists = TestLists();
	// rolls 4 (magic missile), 4 again (a duplicate: re-rolled), 7 (the patron's spell: re-rolled
	// without patron bond), 5 (sleep)
	auto picks = RollArcaneSpells(lists, 1, 2, {}, Wizardly, false, Dice({ 4, 4, 7, 5 }));
	ASSERT_EQ(picks.size(), 2U);
	EXPECT_EQ(picks[0].spell, "dwmmis");
	EXPECT_EQ(picks[0].row, 4);
	EXPECT_FALSE(picks[0].substituted);
	EXPECT_EQ(picks[1].spell, "dwslee");
	EXPECT_EQ(picks[1].row, 5);
	// a spell it knows already is re-rolled too
	picks = RollArcaneSpells(lists, 1, 1, { "dwmmis" }, Wizardly, false, Dice({ 4, 5 }));
	ASSERT_EQ(picks.size(), 1U);
	EXPECT_EQ(picks[0].spell, "dwslee");
}

TEST(DCC_Learning, RowsWithoutContentAreSubstituted)
{
	SpellLists lists = TestLists();
	// row 1 (chill touch) has no content: noted, substituted by the second of the two spells
	// with content (the substitute die rolls 2)
	auto picks = RollArcaneSpells(lists, 1, 1, {}, Wizardly, false, Dice({ 1, 2 }));
	ASSERT_EQ(picks.size(), 1U);
	EXPECT_EQ(picks[0].row, 1);
	EXPECT_EQ(picks[0].rolled, "Chill touch");
	EXPECT_TRUE(picks[0].substituted);
	EXPECT_EQ(picks[0].spell, "dwslee");
	EXPECT_NE(picks[0].note.find("Chill touch has no content yet"), std::string::npos);
	// a cleric spell (wizards can't cast those yet) and the patron pair are substituted too
	picks = RollArcaneSpells(lists, 1, 1, {}, Wizardly, false, Dice({ 2, 1 }));
	EXPECT_TRUE(picks[0].substituted);
	EXPECT_NE(picks[0].note.find("cleric spell"), std::string::npos);
	EXPECT_EQ(picks[0].spell, "dwmmis");
	picks = RollArcaneSpells(lists, 1, 1, {}, Wizardly, false, Dice({ 3, 1 }));
	EXPECT_EQ(picks[0].rolled, "Invoke patron");
	EXPECT_TRUE(picks[0].substituted);

	// four slots, two spells with content: two learned, two open; no row rolled twice
	picks = RollArcaneSpells(lists, 1, 4, {}, Wizardly, false, Dice({ 1, 1, 1, 2, 3, 6 }));
	ASSERT_EQ(picks.size(), 4U);
	int learned = 0;
	int open = 0;
	std::set<int> rows;
	for (const auto& pick : picks) {
		if (pick.spell.empty()) {
			++open;
			EXPECT_NE(pick.note.find("the slot stays open"), std::string::npos);
		} else {
			++learned;
		}
		EXPECT_TRUE(rows.insert(pick.row).second) << "row " << pick.row << " twice";
	}
	EXPECT_EQ(learned, 2);
	EXPECT_EQ(open, 2);
	// nothing ever learned twice, and nothing at all is never a crash
	EXPECT_TRUE(RollArcaneSpells(lists, 2, 3, {}, Wizardly, false, Dice({})).empty());
	picks = RollArcaneSpells(lists, 1, 2, { "dwmmis", "dwslee" }, Wizardly, false, Dice({}));
	ASSERT_EQ(picks.size(), 2U);
	EXPECT_TRUE(picks[0].spell.empty());
	EXPECT_TRUE(picks[1].spell.empty());
}

TEST(DCC_Learning, ClericPicks)
{
	SpellLists lists = TestLists();
	auto clerical = [](const std::string& spell) { return spell == "dcbles" || spell == "dcpara"; };
	// Table 1-5 gives four at level 1; two have content: both picked (the die picks the order), two open
	auto picks = PickClericSpells(lists, 1, 4, {}, clerical, Dice({ 2 }));
	ASSERT_EQ(picks.size(), 4U);
	EXPECT_EQ(picks[0].spell, "dcpara");
	EXPECT_EQ(picks[0].row, 3);
	EXPECT_EQ(picks[1].spell, "dcbles");
	EXPECT_TRUE(picks[2].spell.empty());
	EXPECT_TRUE(picks[3].spell.empty());
	// a known one isn't picked again
	picks = PickClericSpells(lists, 1, 1, { "dcbles" }, clerical, Dice({}));
	EXPECT_EQ(picks[0].spell, "dcpara");
	// the lists by spell
	ASSERT_NE(lists.Find(CasterKind::Cleric, "DCPARA"), nullptr);
	EXPECT_EQ(lists.Find(CasterKind::Cleric, "dcpara")->row, 3);
	EXPECT_EQ(lists.Find(CasterKind::Wizard, "dcbles"), nullptr);
}

}
}
