// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC classes and levels (classes.md): XP to level, the class and level rows,
// hit dice, save totals per class and level, thief skills and the IE save map.
// The shipped 2DAs are checked against the spec in tests/2DAImporter/Test_DCCTables.cpp.

#include "../../../core/DCC/Classes.h"
#include "../../../core/DCC/Rules.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

// Table 1-4
static const std::vector<int> table14 { 0, 10, 50, 110, 190, 290, 410, 550, 710, 890, 1090 };

TEST(DCC_Classes, XPToLevel)
{
	XPTable xp;
	EXPECT_TRUE(xp.IsEmpty());
	ASSERT_TRUE(xp.SetThresholds(table14));
	EXPECT_EQ(xp.MaxLevel(), 10);

	EXPECT_EQ(xp.LevelForXP(0), 0);
	EXPECT_EQ(xp.LevelForXP(9), 0);
	EXPECT_EQ(xp.LevelForXP(10), 1);
	EXPECT_EQ(xp.LevelForXP(49), 1);
	EXPECT_EQ(xp.LevelForXP(50), 2);
	EXPECT_EQ(xp.LevelForXP(189), 3);
	EXPECT_EQ(xp.LevelForXP(190), 4);
	EXPECT_EQ(xp.LevelForXP(1089), 9);
	EXPECT_EQ(xp.LevelForXP(1090), 10);
	EXPECT_EQ(xp.LevelForXP(99999), 10);
	EXPECT_EQ(xp.LevelForXP(-5), 0);
	for (int level = 0; level <= 10; ++level) {
		EXPECT_EQ(xp.XPForLevel(level), table14[level]);
		EXPECT_EQ(xp.LevelForXP(xp.XPForLevel(level)), level);
	}
	EXPECT_EQ(xp.XPForLevel(11), -1);
	EXPECT_EQ(xp.XPForLevel(-1), -1);

	// must start at 0 and rise
	EXPECT_FALSE(xp.SetThresholds({ 5, 10 }));
	EXPECT_FALSE(xp.SetThresholds({ 0, 10, 10 }));
	EXPECT_FALSE(xp.SetThresholds({}));
	EXPECT_TRUE(xp.IsEmpty());
}

static LevelRow Row(int attack, int ref, int fort, int will)
{
	LevelRow row;
	row.attack = attack;
	row.ref = ref;
	row.fort = fort;
	row.will = will;
	return row;
}

// level 0 and two classes, two levels each (classes.md Tables 1-10 and 1-12, funnel.md)
static std::vector<ClassInfo> SmallClasses()
{
	ClassInfo level0 { static_cast<int>(ClassID::Level0), "Level_0", 4, false, 0, { Row(0, 0, 0, 0) } };
	ClassInfo warrior { static_cast<int>(ClassID::Warrior), "Warrior", 12, true, 1, { LevelRow(), Row(0, 1, 1, 0), Row(0, 1, 1, 0) } };
	warrior.levels[1].deedDie = 3;
	warrior.levels[2].deedDie = 4;
	warrior.levels[1].threat = 19;
	ClassInfo wizard { static_cast<int>(ClassID::Wizard), "Wizard", 4, false, 1, { LevelRow(), Row(0, 1, 0, 1), Row(1, 1, 0, 1) } };
	return { level0, warrior, wizard };
}

TEST(DCC_Classes, ClassRows)
{
	ClassTable classes;
	EXPECT_TRUE(classes.IsEmpty());
	EXPECT_EQ(classes.Row(2, 1), nullptr);
	ASSERT_TRUE(classes.SetClasses(SmallClasses()));

	EXPECT_TRUE(classes.IsCharacterClass(static_cast<int>(ClassID::Warrior)));
	EXPECT_TRUE(classes.IsCharacterClass(static_cast<int>(ClassID::Level0)));
	EXPECT_FALSE(classes.IsCharacterClass(static_cast<int>(ClassID::Thief))); // not in this table
	EXPECT_FALSE(classes.IsCharacterClass(155)); // the demo protagonist, a monster class to us
	EXPECT_FALSE(classes.IsCharacterClass(0));

	const LevelRow* row = classes.Row(static_cast<int>(ClassID::Warrior), 1);
	ASSERT_NE(row, nullptr);
	EXPECT_EQ(row->deedDie, 3);
	EXPECT_EQ(row->threat, 19);
	// past the table clamps to the last level
	EXPECT_EQ(classes.Row(static_cast<int>(ClassID::Warrior), 7)->deedDie, 4);
	// level 0 is the 0-level row, whatever the class
	EXPECT_EQ(classes.Row(static_cast<int>(ClassID::Warrior), 0), classes.Row(static_cast<int>(ClassID::Level0), 0));
	EXPECT_EQ(classes.Row(static_cast<int>(ClassID::Level0), 3), classes.Row(static_cast<int>(ClassID::Level0), 0));
	EXPECT_EQ(classes.Row(155, 1), nullptr);

	EXPECT_EQ(classes.HitDie(static_cast<int>(ClassID::Warrior), 1), 12);
	EXPECT_EQ(classes.HitDie(static_cast<int>(ClassID::Wizard), 2), 4);
	EXPECT_EQ(classes.HitDie(static_cast<int>(ClassID::Warrior), 0), 4);
	EXPECT_EQ(classes.HitDie(static_cast<int>(ClassID::Level0), 0), 4);
	EXPECT_EQ(classes.Class(static_cast<int>(ClassID::Wizard))->race, 1);
}

TEST(DCC_Classes, BadClassTables)
{
	ClassTable classes;
	auto good = SmallClasses();

	auto noLevel0 = good;
	noLevel0.erase(noLevel0.begin());
	EXPECT_FALSE(classes.SetClasses(noLevel0));

	auto twice = good;
	twice.push_back(good[1]);
	EXPECT_FALSE(classes.SetClasses(twice));

	auto unknown = good;
	unknown[1].id = 42;
	EXPECT_FALSE(classes.SetClasses(unknown));

	auto noLevels = good;
	noLevels[2].levels.resize(1);
	EXPECT_FALSE(classes.SetClasses(noLevels));

	auto badLevel0 = good;
	badLevel0[0].levels.push_back(LevelRow());
	EXPECT_FALSE(classes.SetClasses(badLevel0));

	auto noHitDie = good;
	noHitDie[1].hitDie = 0;
	EXPECT_FALSE(classes.SetClasses(noHitDie));
	EXPECT_TRUE(classes.IsEmpty());
}

TEST(DCC_Classes, SaveTotalsPerClassAndLevel)
{
	ClassTable classes;
	ASSERT_TRUE(classes.SetClasses(SmallClasses()));
	auto totals = [&classes](ClassID id, int level, int sta, int agi, int per) {
		const LevelRow* row = classes.Row(static_cast<int>(id), level);
		return std::array<int, 3> {
			SaveTotal(row->SaveBonus(Save::Fort), sta, 0, 0),
			SaveTotal(row->SaveBonus(Save::Ref), agi, 0, 0),
			SaveTotal(row->SaveBonus(Save::Will), per, 0, 0)
		};
	};
	// Grub, all 3s (mod -3), at level 0
	EXPECT_EQ(totals(ClassID::Level0, 0, -3, -3, -3), (std::array<int, 3> { -3, -3, -3 }));
	// Ilsa, all 18s (mod +3), at level 0
	EXPECT_EQ(totals(ClassID::Level0, 0, 3, 3, 3), (std::array<int, 3> { 3, 3, 3 }));
	// a warrior at level 1: Ref +1, Fort +1, Will +0
	EXPECT_EQ(totals(ClassID::Warrior, 1, 1, 0, -1), (std::array<int, 3> { 2, 1, -1 }));
	// a wizard at level 2: Ref +1, Fort +0, Will +1
	EXPECT_EQ(totals(ClassID::Wizard, 2, 0, 2, 1), (std::array<int, 3> { 0, 3, 2 }));
}

TEST(DCC_Classes, ThiefSkills)
{
	ThiefSkillTable thief;
	EXPECT_TRUE(thief.IsEmpty());
	EXPECT_EQ(thief.Bonus(Alignment::Lawful, 1, ThiefSkill::Hide), 0);

	ThiefSkillTable::Row level1 {};
	level1[static_cast<size_t>(ThiefSkill::Hide)] = 3;
	ThiefSkillTable::Row level2 {};
	level2[static_cast<size_t>(ThiefSkill::Hide)] = 5;
	for (Alignment path : { Alignment::Lawful, Alignment::Chaotic, Alignment::Neutral }) {
		ASSERT_TRUE(thief.SetRows(path, { level1, level2 }));
	}
	EXPECT_FALSE(thief.IsEmpty());
	EXPECT_EQ(thief.Bonus(Alignment::Lawful, 1, ThiefSkill::Hide), 3);
	EXPECT_EQ(thief.Bonus(Alignment::Lawful, 2, ThiefSkill::Hide), 5);
	EXPECT_EQ(thief.Bonus(Alignment::Lawful, 9, ThiefSkill::Hide), 5);
	EXPECT_EQ(thief.Bonus(Alignment::Lawful, 0, ThiefSkill::Hide), 0);

	Ability ability;
	ASSERT_TRUE(ThiefSkillAbility(ThiefSkill::Hide, ability));
	EXPECT_EQ(ability, Ability::Agi);
	ASSERT_TRUE(ThiefSkillAbility(ThiefSkill::FindTrap, ability));
	EXPECT_EQ(ability, Ability::Int);
	ASSERT_TRUE(ThiefSkillAbility(ThiefSkill::DisableTrap, ability));
	EXPECT_EQ(ability, Ability::Agi);
	ASSERT_TRUE(ThiefSkillAbility(ThiefSkill::Disguise, ability));
	EXPECT_EQ(ability, Ability::Per);
	ASSERT_TRUE(ThiefSkillAbility(ThiefSkill::ReadLanguages, ability));
	EXPECT_EQ(ability, Ability::Int);
	EXPECT_FALSE(ThiefSkillAbility(ThiefSkill::Backstab, ability));
	EXPECT_FALSE(ThiefSkillAbility(ThiefSkill::HandlePoison, ability));
}

TEST(DCC_Classes, IESaveMap)
{
	SaveMap map;
	// stock mapping: spell Will, breath Ref, death Fort, wand Ref, polymorph Fort
	EXPECT_EQ(map.Get(IESave::Spell), Save::Will);
	EXPECT_EQ(map.Get(IESave::Breath), Save::Ref);
	EXPECT_EQ(map.Get(IESave::Death), Save::Fort);
	EXPECT_EQ(map.Get(IESave::Wand), Save::Ref);
	EXPECT_EQ(map.Get(IESave::Polymorph), Save::Fort);
	EXPECT_EQ(map.Get(7U), map.Default());

	Save save = Save::Fort;
	EXPECT_TRUE(SaveMap::ParseSave("WILL", save));
	EXPECT_EQ(save, Save::Will);
	EXPECT_FALSE(SaveMap::ParseSave("DEATH", save));
	EXPECT_STREQ(SaveMap::SaveKey(Save::Ref), "REF");
}

}
}
