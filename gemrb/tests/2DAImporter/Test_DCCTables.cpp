// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Checks the shipped DCC tables in unhardcoded/ethren against the spec
// (docs/rules/classes.md Tables 1-4 to 1-18, luck.md Table 1-2, funnel.md),
// read through the real 2DA importer and the DCC loaders.

#include "../../core/DCC/Deeds.h"
#include "../../core/DCC/Rules.h"
#include "../../core/Streams/FileStream.h"
#include "../../plugins/2DAImporter/2DAImporter.h"

#include <gtest/gtest.h>

#include <set>

namespace GemRB {
namespace DCC {

static std::unique_ptr<p2DAImporter> Open2DA(const std::string& name)
{
	std::string lower = name;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
	path_t file = PathJoin(GEMRB_UNHARDCODED_DIR, "ethren", lower + ".2da");
	auto stream = std::make_unique<FileStream>();
	if (!stream->Open(file)) {
		ADD_FAILURE() << "can't open " << file;
		return nullptr;
	}
	auto importer = std::make_unique<p2DAImporter>();
	if (!importer->Open(std::unique_ptr<DataStream> { std::move(stream) })) {
		ADD_FAILURE() << "can't read " << file;
		return nullptr;
	}
	return importer;
}

TEST(DCC_Tables, XPMatchesTable14)
{
	auto table = Open2DA("dccxp");
	ASSERT_TRUE(table);
	XPTable xp;
	ASSERT_TRUE(xp.Load(*table));
	const int expected[] = { 0, 10, 50, 110, 190, 290, 410, 550, 710, 890, 1090 };
	ASSERT_EQ(xp.MaxLevel(), 10);
	for (int level = 0; level <= 10; ++level) {
		EXPECT_EQ(xp.XPForLevel(level), expected[level]) << "level " << level;
	}
}

static ClassTable LoadClasses()
{
	ClassTable classes;
	auto list = Open2DA("dccclass");
	EXPECT_TRUE(list);
	if (list) {
		EXPECT_TRUE(classes.Load(*list, [](const std::string& name) { return Open2DA(name); }));
	}
	return classes;
}

// a progression row as the spec prints it: attack (or deed die and its fixed part), crit
// dice d die + add / table, threat, action dice, Ref, Fort, Will
struct SpecRow {
	int attack;
	int deedDie;
	int deedAdd;
	int critDice;
	int critDie;
	int critAdd;
	int critTable;
	int threat;
	std::array<int, 3> act;
	int ref;
	int fort;
	int will;
};

static void ExpectRows(const ClassTable& classes, ClassID id, const std::vector<SpecRow>& spec)
{
	for (size_t i = 0; i < spec.size(); ++i) {
		int level = int(i) + 1;
		const LevelRow* row = classes.Row(static_cast<int>(id), level);
		ASSERT_NE(row, nullptr);
		const SpecRow& s = spec[i];
		SCOPED_TRACE(testing::Message() << "class " << static_cast<int>(id) << " level " << level);
		EXPECT_EQ(row->attack, s.attack);
		EXPECT_EQ(row->deedDie, s.deedDie);
		EXPECT_EQ(row->deedAdd, s.deedAdd);
		EXPECT_EQ(row->critDice, s.critDice);
		EXPECT_EQ(row->critDie, s.critDie);
		EXPECT_EQ(row->critAdd, s.critAdd);
		EXPECT_EQ(row->critTable, s.critTable);
		EXPECT_EQ(row->threat, s.threat);
		EXPECT_EQ(row->actionDice, s.act);
		EXPECT_EQ(row->ref, s.ref);
		EXPECT_EQ(row->fort, s.fort);
		EXPECT_EQ(row->will, s.will);
	}
}

TEST(DCC_Tables, ClassList)
{
	ClassTable classes = LoadClasses();
	ASSERT_FALSE(classes.IsEmpty());
	// id, hit die, deed die, race needed
	const int expected[8][4] = {
		{ 1, 4, 0, 1 }, // wizard
		{ 2, 12, 1, 1 }, // warrior
		{ 3, 8, 0, 1 }, // cleric
		{ 4, 6, 0, 1 }, // thief
		{ 5, 10, 1, 4 }, // dwarf
		{ 6, 6, 0, 2 }, // elf
		{ 7, 6, 0, 5 }, // halfling
		{ 8, 4, 0, 0 } // level 0: 1d4
	};
	for (const auto& e : expected) {
		const ClassInfo* info = classes.Class(e[0]);
		ASSERT_NE(info, nullptr) << e[0];
		EXPECT_EQ(info->hitDie, e[1]) << e[0];
		EXPECT_EQ(info->deed, e[2] != 0) << e[0];
		EXPECT_EQ(info->race, e[3]) << e[0];
		EXPECT_EQ(int(info->levels.size()), e[0] == 8 ? 1 : 11) << e[0];
	}
}

TEST(DCC_Tables, Level0)
{
	ClassTable classes = LoadClasses();
	const LevelRow* row = classes.Row(static_cast<int>(ClassID::Level0), 0);
	ASSERT_NE(row, nullptr);
	// +0 attack, 1d4 on crit table I, a d20, +0 saves
	EXPECT_EQ(row->attack, 0);
	EXPECT_EQ(row->critDice, 1);
	EXPECT_EQ(row->critDie, 4);
	EXPECT_EQ(row->critTable, 1);
	EXPECT_EQ(row->threat, 20);
	EXPECT_EQ(row->actionDice, (std::array<int, 3> { 20, 0, 0 }));
	EXPECT_EQ(row->ref + row->fort + row->will, 0);
}

TEST(DCC_Tables, Cleric)
{
	ClassTable classes = LoadClasses();
	ExpectRows(classes, ClassID::Cleric, {
		{ 0, 0, 0, 1, 8, 0, 3, 20, { 20, 0, 0 }, 0, 1, 1 },
		{ 1, 0, 0, 1, 8, 0, 3, 20, { 20, 0, 0 }, 0, 1, 1 },
		{ 2, 0, 0, 1, 10, 0, 3, 20, { 20, 0, 0 }, 1, 1, 2 },
		{ 2, 0, 0, 1, 10, 0, 3, 20, { 20, 0, 0 }, 1, 2, 2 },
		{ 3, 0, 0, 1, 12, 0, 3, 20, { 20, 0, 0 }, 1, 2, 3 },
		{ 4, 0, 0, 1, 12, 0, 3, 20, { 20, 14, 0 }, 2, 2, 4 },
		{ 5, 0, 0, 1, 14, 0, 3, 20, { 20, 16, 0 }, 2, 3, 4 },
		{ 5, 0, 0, 1, 14, 0, 3, 20, { 20, 20, 0 }, 2, 3, 5 },
		{ 6, 0, 0, 1, 16, 0, 3, 20, { 20, 20, 0 }, 3, 3, 5 },
		{ 7, 0, 0, 1, 16, 0, 3, 20, { 20, 20, 0 }, 3, 4, 6 },
	});
}

TEST(DCC_Tables, Thief)
{
	ClassTable classes = LoadClasses();
	ExpectRows(classes, ClassID::Thief, {
		{ 0, 0, 0, 1, 10, 0, 2, 20, { 20, 0, 0 }, 1, 1, 0 },
		{ 1, 0, 0, 1, 12, 0, 2, 20, { 20, 0, 0 }, 1, 1, 0 },
		{ 2, 0, 0, 1, 14, 0, 2, 20, { 20, 0, 0 }, 2, 1, 1 },
		{ 2, 0, 0, 1, 16, 0, 2, 20, { 20, 0, 0 }, 2, 2, 1 },
		{ 3, 0, 0, 1, 20, 0, 2, 20, { 20, 0, 0 }, 3, 2, 1 },
		{ 4, 0, 0, 1, 24, 0, 2, 20, { 20, 14, 0 }, 4, 2, 2 },
		{ 5, 0, 0, 1, 30, 0, 2, 20, { 20, 16, 0 }, 4, 3, 2 },
		{ 5, 0, 0, 1, 30, 2, 2, 20, { 20, 20, 0 }, 5, 3, 2 },
		{ 6, 0, 0, 1, 30, 4, 2, 20, { 20, 20, 0 }, 5, 3, 3 },
		{ 7, 0, 0, 1, 30, 6, 2, 20, { 20, 20, 0 }, 6, 4, 3 },
	});
	const int luckDie[] = { 3, 4, 5, 6, 7, 8, 10, 12, 14, 16 };
	for (int level = 1; level <= 10; ++level) {
		EXPECT_EQ(classes.Row(static_cast<int>(ClassID::Thief), level)->luckDie, luckDie[level - 1]) << level;
	}
}

TEST(DCC_Tables, Warrior)
{
	ClassTable classes = LoadClasses();
	ExpectRows(classes, ClassID::Warrior, {
		{ 0, 3, 0, 1, 12, 0, 3, 19, { 20, 0, 0 }, 1, 1, 0 },
		{ 0, 4, 0, 1, 14, 0, 3, 19, { 20, 0, 0 }, 1, 1, 0 },
		{ 0, 5, 0, 1, 16, 0, 4, 19, { 20, 0, 0 }, 1, 2, 1 },
		{ 0, 6, 0, 1, 20, 0, 4, 19, { 20, 0, 0 }, 2, 2, 1 },
		{ 0, 7, 0, 1, 24, 0, 5, 18, { 20, 14, 0 }, 2, 3, 1 },
		{ 0, 8, 0, 1, 30, 0, 5, 18, { 20, 16, 0 }, 2, 4, 2 },
		{ 0, 10, 1, 1, 30, 0, 5, 18, { 20, 20, 0 }, 3, 4, 2 },
		{ 0, 10, 2, 2, 20, 0, 5, 18, { 20, 20, 0 }, 3, 5, 2 },
		{ 0, 10, 3, 2, 20, 0, 5, 17, { 20, 20, 0 }, 3, 5, 3 },
		{ 0, 10, 4, 2, 20, 0, 5, 17, { 20, 20, 14 }, 4, 6, 3 },
	});
}

TEST(DCC_Tables, Wizard)
{
	ClassTable classes = LoadClasses();
	ExpectRows(classes, ClassID::Wizard, {
		{ 0, 0, 0, 1, 6, 0, 1, 20, { 20, 0, 0 }, 1, 0, 1 },
		{ 1, 0, 0, 1, 6, 0, 1, 20, { 20, 0, 0 }, 1, 0, 1 },
		{ 1, 0, 0, 1, 8, 0, 1, 20, { 20, 0, 0 }, 1, 1, 2 },
		{ 1, 0, 0, 1, 8, 0, 1, 20, { 20, 0, 0 }, 2, 1, 2 },
		{ 2, 0, 0, 1, 10, 0, 1, 20, { 20, 14, 0 }, 2, 1, 3 },
		{ 2, 0, 0, 1, 10, 0, 1, 20, { 20, 16, 0 }, 2, 2, 4 },
		{ 3, 0, 0, 1, 12, 0, 1, 20, { 20, 20, 0 }, 3, 2, 4 },
		{ 3, 0, 0, 1, 12, 0, 1, 20, { 20, 20, 0 }, 3, 2, 5 },
		{ 4, 0, 0, 1, 14, 0, 1, 20, { 20, 20, 0 }, 3, 3, 5 },
		{ 4, 0, 0, 1, 14, 0, 1, 20, { 20, 20, 14 }, 4, 3, 6 },
	});
	const int known[] = { 4, 5, 6, 7, 8, 9, 10, 12, 14, 16 };
	const int maxLevel[] = { 1, 1, 2, 2, 3, 3, 4, 4, 5, 5 };
	for (int level = 1; level <= 10; ++level) {
		EXPECT_EQ(classes.Row(static_cast<int>(ClassID::Wizard), level)->spellsKnown, known[level - 1]) << level;
		EXPECT_EQ(classes.Row(static_cast<int>(ClassID::Wizard), level)->maxSpellLevel, maxLevel[level - 1]) << level;
	}
}

TEST(DCC_Tables, Dwarf)
{
	ClassTable classes = LoadClasses();
	ExpectRows(classes, ClassID::Dwarf, {
		{ 0, 3, 0, 1, 10, 0, 3, 20, { 20, 0, 0 }, 1, 1, 1 },
		{ 0, 4, 0, 1, 12, 0, 3, 20, { 20, 0, 0 }, 1, 1, 1 },
		{ 0, 5, 0, 1, 14, 0, 3, 20, { 20, 0, 0 }, 1, 2, 1 },
		{ 0, 6, 0, 1, 16, 0, 4, 20, { 20, 0, 0 }, 2, 2, 2 },
		{ 0, 7, 0, 1, 20, 0, 4, 20, { 20, 14, 0 }, 2, 3, 2 },
		{ 0, 8, 0, 1, 24, 0, 5, 20, { 20, 16, 0 }, 2, 4, 2 },
		{ 0, 10, 1, 1, 30, 0, 5, 20, { 20, 20, 0 }, 3, 4, 3 },
		{ 0, 10, 2, 1, 30, 0, 5, 20, { 20, 20, 0 }, 3, 5, 3 },
		{ 0, 10, 3, 2, 20, 0, 5, 20, { 20, 20, 0 }, 3, 5, 3 },
		{ 0, 10, 4, 2, 20, 0, 5, 20, { 20, 20, 14 }, 4, 6, 4 },
	});
}

TEST(DCC_Tables, Elf)
{
	ClassTable classes = LoadClasses();
	ExpectRows(classes, ClassID::Elf, {
		{ 1, 0, 0, 1, 6, 0, 2, 20, { 20, 0, 0 }, 1, 1, 1 },
		{ 1, 0, 0, 1, 8, 0, 2, 20, { 20, 0, 0 }, 1, 1, 1 },
		{ 2, 0, 0, 1, 8, 0, 2, 20, { 20, 0, 0 }, 1, 1, 2 },
		{ 2, 0, 0, 1, 10, 0, 2, 20, { 20, 0, 0 }, 2, 2, 2 },
		{ 3, 0, 0, 1, 10, 0, 2, 20, { 20, 14, 0 }, 2, 2, 3 },
		{ 3, 0, 0, 1, 12, 0, 2, 20, { 20, 16, 0 }, 2, 2, 4 },
		{ 4, 0, 0, 1, 12, 0, 2, 20, { 20, 20, 0 }, 3, 3, 4 },
		{ 4, 0, 0, 1, 14, 0, 2, 20, { 20, 20, 0 }, 3, 3, 5 },
		{ 5, 0, 0, 1, 14, 0, 2, 20, { 20, 20, 0 }, 3, 3, 5 },
		{ 5, 0, 0, 1, 16, 0, 2, 20, { 20, 20, 14 }, 4, 4, 6 },
	});
	const int known[] = { 3, 4, 5, 6, 7, 8, 9, 10, 12, 14 };
	for (int level = 1; level <= 10; ++level) {
		EXPECT_EQ(classes.Row(static_cast<int>(ClassID::Elf), level)->spellsKnown, known[level - 1]) << level;
	}
}

TEST(DCC_Tables, Halfling)
{
	ClassTable classes = LoadClasses();
	ExpectRows(classes, ClassID::Halfling, {
		{ 1, 0, 0, 1, 8, 0, 3, 20, { 20, 0, 0 }, 1, 1, 1 },
		{ 2, 0, 0, 1, 8, 0, 3, 20, { 20, 0, 0 }, 1, 1, 1 },
		{ 2, 0, 0, 1, 10, 0, 3, 20, { 20, 0, 0 }, 2, 1, 2 },
		{ 3, 0, 0, 1, 10, 0, 3, 20, { 20, 0, 0 }, 2, 2, 2 },
		{ 4, 0, 0, 1, 12, 0, 3, 20, { 20, 0, 0 }, 3, 2, 3 },
		{ 5, 0, 0, 1, 12, 0, 3, 20, { 20, 14, 0 }, 4, 2, 4 },
		{ 5, 0, 0, 1, 14, 0, 3, 20, { 20, 16, 0 }, 4, 3, 4 },
		{ 6, 0, 0, 1, 14, 0, 3, 20, { 20, 20, 0 }, 5, 3, 5 },
		{ 7, 0, 0, 1, 16, 0, 3, 20, { 20, 20, 0 }, 5, 3, 5 },
		{ 8, 0, 0, 1, 16, 0, 3, 20, { 20, 20, 0 }, 6, 4, 6 },
	});
	const int sneakHide[] = { 3, 5, 7, 8, 9, 11, 12, 13, 14, 15 };
	for (int level = 1; level <= 10; ++level) {
		EXPECT_EQ(classes.Row(static_cast<int>(ClassID::Halfling), level)->sneakHide, sneakHide[level - 1]) << level;
	}
}

TEST(DCC_Tables, ThiefSkillsMatchTable19)
{
	ThiefSkillTable thief;
	const std::pair<Alignment, const char*> paths[] = { { Alignment::Lawful, "dccthl" }, { Alignment::Chaotic, "dccthc" }, { Alignment::Neutral, "dccthn" } };
	for (const auto& path : paths) {
		auto table = Open2DA(path.second);
		ASSERT_TRUE(table);
		ASSERT_TRUE(thief.Load(path.first, *table)) << path.second;
	}

	// levels 1, 5 and 10 of every skill, per path (lawful, chaotic, neutral)
	struct Expect {
		ThiefSkill skill;
		int lawful[3];
		int chaotic[3];
		int neutral[3];
	};
	const Expect expected[] = {
		{ ThiefSkill::Backstab, { 1, 8, 13 }, { 3, 9, 15 }, { 0, 4, 9 } },
		{ ThiefSkill::Sneak, { 1, 8, 13 }, { 3, 9, 15 }, { 3, 9, 15 } },
		{ ThiefSkill::Hide, { 3, 9, 15 }, { 1, 8, 13 }, { 1, 8, 13 } },
		{ ThiefSkill::PickPocket, { 1, 8, 13 }, { 0, 4, 9 }, { 3, 9, 15 } },
		{ ThiefSkill::Climb, { 3, 9, 15 }, { 1, 8, 13 }, { 3, 9, 15 } },
		{ ThiefSkill::PickLock, { 1, 8, 13 }, { 1, 8, 13 }, { 1, 8, 13 } },
		{ ThiefSkill::FindTrap, { 3, 9, 15 }, { 1, 8, 13 }, { 1, 8, 13 } },
		{ ThiefSkill::DisableTrap, { 3, 9, 15 }, { 0, 4, 9 }, { 1, 8, 13 } },
		{ ThiefSkill::Forge, { 0, 3, 8 }, { 0, 3, 8 }, { 3, 9, 15 } },
		{ ThiefSkill::Disguise, { 0, 4, 9 }, { 3, 9, 15 }, { 0, 3, 8 } },
		{ ThiefSkill::ReadLanguages, { 0, 3, 8 }, { 0, 3, 8 }, { 0, 4, 9 } },
		{ ThiefSkill::HandlePoison, { 0, 4, 9 }, { 3, 9, 15 }, { 0, 3, 8 } },
		{ ThiefSkill::ScrollDie, { 10, 14, 20 }, { 10, 14, 20 }, { 12, 16, 20 } },
	};
	const int levels[] = { 1, 5, 10 };
	for (const Expect& e : expected) {
		for (int i = 0; i < 3; ++i) {
			EXPECT_EQ(thief.Bonus(Alignment::Lawful, levels[i], e.skill), e.lawful[i]) << ThiefSkillKey(e.skill) << " " << levels[i];
			EXPECT_EQ(thief.Bonus(Alignment::Chaotic, levels[i], e.skill), e.chaotic[i]) << ThiefSkillKey(e.skill) << " " << levels[i];
			EXPECT_EQ(thief.Bonus(Alignment::Neutral, levels[i], e.skill), e.neutral[i]) << ThiefSkillKey(e.skill) << " " << levels[i];
		}
	}
	// a whole curve: lawful hide in shadows is the A curve
	const int curveA[] = { 3, 5, 7, 8, 9, 11, 12, 13, 14, 15 };
	for (int level = 1; level <= 10; ++level) {
		EXPECT_EQ(thief.Bonus(Alignment::Lawful, level, ThiefSkill::Hide), curveA[level - 1]) << level;
	}
}

TEST(DCC_Tables, AugursMatchTable12)
{
	auto table = Open2DA("dccaugur");
	ASSERT_TRUE(table);
	AugurTable augurs;
	ASSERT_TRUE(augurs.Load(*table));
	ASSERT_EQ(augurs.Count(), 30);
	const AugurTarget expected[30] = {
		AugurTarget::AttackAll, AugurTarget::AttackMelee, AugurTarget::AttackMissile, AugurTarget::AttackUnarmed,
		AugurTarget::AttackMounted, AugurTarget::DamageAll, AugurTarget::DamageMelee, AugurTarget::DamageMissile,
		AugurTarget::OccupationWeapon, AugurTarget::SkillChecks, AugurTarget::Traps, AugurTarget::SecretDoors,
		AugurTarget::SpellChecks, AugurTarget::SpellDamage, AugurTarget::TurnUnholy, AugurTarget::Healing,
		AugurTarget::SavesAll, AugurTarget::SavesTraps, AugurTarget::SavesPoison, AugurTarget::SaveRef,
		AugurTarget::SaveFort, AugurTarget::SaveWill, AugurTarget::ArmorClass, AugurTarget::Initiative,
		AugurTarget::HitPoints, AugurTarget::CritTables, AugurTarget::Corruption, AugurTarget::Fumbles,
		AugurTarget::Languages, AugurTarget::Speed
	};
	for (int augur = 1; augur <= 30; ++augur) {
		EXPECT_EQ(augurs.Target(augur), expected[augur - 1]) << augur;
		EXPECT_FALSE(augurs.Name(augur).empty()) << augur;
	}
	EXPECT_EQ(augurs.Name(17), "Lucky_sign");
}

TEST(DCC_Tables, SaveMap)
{
	auto table = Open2DA("dccsave");
	ASSERT_TRUE(table);
	SaveMap saves;
	ASSERT_TRUE(saves.Load(*table));
	EXPECT_EQ(saves.Get(IESave::Spell), Save::Will);
	EXPECT_EQ(saves.Get(IESave::Breath), Save::Ref);
	EXPECT_EQ(saves.Get(IESave::Death), Save::Fort);
	EXPECT_EQ(saves.Get(IESave::Wand), Save::Ref);
	EXPECT_EQ(saves.Get(IESave::Polymorph), Save::Fort);
	EXPECT_EQ(saves.Default(), Save::Will);
}

// --- combat (M2.3): crits-fumbles.md, combat.md Table 4-3, equipment.md Table 3-3

// the extra damage dice of each roll, as the spec's tables print them ("" for none)
static void ExpectDice(const char* name, int minRoll, const std::vector<std::string>& dice)
{
	auto table = Open2DA(name);
	ASSERT_TRUE(table) << name;
	ResultTable results;
	ASSERT_TRUE(results.Load(*table)) << name;
	EXPECT_EQ(results.MinRoll(), minRoll) << name;
	EXPECT_EQ(results.MaxRoll(), minRoll + int(dice.size()) - 1) << name;
	for (size_t i = 0; i < dice.size(); ++i) {
		int roll = minRoll + int(i);
		const ResultRow* row = results.Lookup(roll);
		ASSERT_TRUE(row);
		EXPECT_EQ(row->roll, roll);
		std::string got = row->dice.IsNone() ? "" : DiceString(row->dice);
		EXPECT_EQ(got, dice[i]) << name << " roll " << roll;
		EXPECT_FALSE(row->text.empty()) << name << " roll " << roll;
	}
}

TEST(DCC_Tables, CritTableI)
{
	ExpectDice("dcccrit1", 0, { "1d6", "1d3", "1d4", "1d3", "1d4", "1d6", "", "2d3", "", "2d4", "", "2d4", "", "", "", "", "", "", "2d6", "", "2d6" });
}

TEST(DCC_Tables, CritTableII)
{
	ExpectDice("dcccrit2", 0, { "", "2d3", "1d6", "2d6", "", "3d3", "", "3d4", "", "", "2d6", "", "", "", "", "2d6", "", "2d6", "1d8", "1d10", "2d6", "3d6", "2d6", "", "3d6" });
}

TEST(DCC_Tables, CritTableIII)
{
	ExpectDice("dcccrit3", 0, { "1d12", "1d6", "", "1d8", "", "1d6", "1d8", "", "1d6", "2d6", "", "", "", "1d8", "2d8", "1d8", "", "", "1d8", "2d8", "1d8", "2d8", "", "2d8", "2d8", "", "2d8", "3d8", "3d8" });
}

TEST(DCC_Tables, CritTableIV)
{
	ExpectDice("dcccrit4", 0, { "2d8", "2d12", "1d12", "1d12", "1d12", "", "", "2d12", "1d8", "2d12", "", "2d12", "1d12", "2d12", "", "", "2d12", "", "1d12", "", "3d12", "", "", "3d12", "3d12" });
}

TEST(DCC_Tables, CritTableV)
{
	ExpectDice("dcccrit5", 0, { "3d8", "", "", "1d12", "", "1d12", "", "", "1d12", "1d12", "1d12", "1d12", "1d12", "", "", "2d12", "", "", "", "", "2d12", "3d12", "2d12", "", "2d12", "", "", "4d12", "" });
}

TEST(DCC_Tables, CritTableM)
{
	ExpectDice("dcccritm", 1, { "1d6", "", "", "", "", "2d6", "1d8", "1d6", "", "", "", "1d10", "1d12", "", "1d12", "1d16", "", "1d12", "1d16", "", "", "2d12", "3d12", "", "", "", "", "", "", "" });
}

TEST(DCC_Tables, FumbleTable)
{
	ExpectDice("dccfumbl", 0, { "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "" });
}

// the saves the spec gives, as table name, roll, save, DC, + level
TEST(DCC_Tables, CritAndFumbleSaves)
{
	struct Expected {
		const char* table;
		int roll;
		Save save;
		int dc;
		bool plusLevel;
	};
	const Expected saves[] = {
		{ "dcccrit1", 12, Save::Fort, 15, false }, { "dcccrit1", 19, Save::Fort, 20, false },
		{ "dcccrit2", 12, Save::Fort, 10, true }, { "dcccrit2", 14, Save::Fort, 10, true }, { "dcccrit2", 18, Save::Fort, 10, true },
		{ "dcccrit2", 19, Save::Fort, 10, true }, { "dcccrit2", 20, Save::Fort, 13, true }, { "dcccrit2", 21, Save::Fort, 15, true },
		{ "dcccrit2", 22, Save::Fort, 13, true }, { "dcccrit2", 23, Save::Fort, 15, true }, { "dcccrit2", 24, Save::Fort, 20, true },
		{ "dcccrit3", 12, Save::Fort, 10, true }, { "dcccrit3", 27, Save::Fort, 15, true }, { "dcccrit3", 28, Save::Fort, 15, true },
		{ "dcccrit4", 6, Save::Fort, 15, true }, { "dcccrit4", 10, Save::Fort, 15, true }, { "dcccrit4", 18, Save::Fort, 15, true },
		{ "dcccrit4", 20, Save::Fort, 15, true }, { "dcccrit4", 23, Save::Fort, 15, true }, { "dcccrit4", 24, Save::Fort, 20, true },
		{ "dcccrit5", 4, Save::Fort, 20, true }, { "dcccrit5", 15, Save::Fort, 20, true },
		{ "dcccritm", 9, Save::Fort, 10, true }, { "dcccritm", 15, Save::Fort, 15, true }, { "dcccritm", 19, Save::Fort, 15, true },
		{ "dccfumbl", 2, Save::Ref, 10, false },
	};
	for (const auto& expected : saves) {
		auto table = Open2DA(expected.table);
		ASSERT_TRUE(table);
		ResultTable results;
		ASSERT_TRUE(results.Load(*table));
		const ResultRow* row = results.Lookup(expected.roll);
		ASSERT_TRUE(row);
		EXPECT_EQ(row->save.save, expected.save) << expected.table << " " << expected.roll;
		EXPECT_EQ(row->save.dc, expected.dc) << expected.table << " " << expected.roll;
		EXPECT_EQ(row->save.plusLevel, expected.plusLevel) << expected.table << " " << expected.roll;
	}
}

// every result key in the shipped tables is one the engine knows (ActorDCCCombat.cpp)
TEST(DCC_Tables, CritAndFumbleKeysKnown)
{
	static const std::set<std::string> known { "NONE", "DMG", "PRONE", "NOATTACK", "INITBOTTOM", "STUN", "BLIND", "DEAF", "MUTE", "UNCON",
						   "PARALYZE", "HELD", "SPEED", "ATK", "AC", "NEXTATK", "ENEMYBONUS", "DIEIN", "DIE", "HALFHP",
						   "ABIL", "DISARM", "DISARMSELF", "SUNDER", "SUNDERSELF", "SHIELD", "WEAPONDMG", "ALLY", "SELF",
						   "SELFHIT", "HITALLY", "DELAYDMG", "EXTRA", "TODO",
						   // M2.3b
						   "INJURY", "RAGE", "HALFACT", "ONEACTION", "FOUL", "DOWNED", "STUCK", "WOUND", "CHOKE", "NUMB",
						   "HALFDMG", "DOUBLEDMG", "SLEEP", "ROLLMOD", "MAXHP", "XP", "REROLL", "ABILTO", "LIMBS", "SMASHGEAR",
						   "FLING", "CLEAVE", "RAMPAGE", "AGAIN", "MORALECHECK", "DRAIN", "ATTACKER",
						   // M2.5
						   "CORRUPT" };
	static const std::set<std::string> conditions { "", "NOHELM", "NOSHIELD", "ARMORED", "HASWEAPON" };
	for (const char* name : { "dcccrit1", "dcccrit2", "dcccrit3", "dcccrit4", "dcccrit5", "dcccritm", "dccfumbl", "dcccritd", "dcccritn", "dcccritg", "dcccritu" }) {
		auto table = Open2DA(name);
		ASSERT_TRUE(table);
		ResultTable results;
		ASSERT_TRUE(results.Load(*table)) << name;
		for (int roll = results.MinRoll(); roll <= results.MaxRoll(); ++roll) {
			for (const auto& alternatives : results.Lookup(roll)->effects) {
				for (const auto& term : alternatives) {
					EXPECT_TRUE(known.count(term.key)) << name << " " << roll << ": " << term.key;
					EXPECT_TRUE(conditions.count(term.condition)) << name << " " << roll << ": @" << term.condition;
					if (term.key == "STUN" || term.key == "PRONE" || term.key == "BLIND" || term.key == "UNCON" || term.key == "DIEIN") {
						Duration duration;
						EXPECT_TRUE(ParseDuration(term.args.empty() ? "" : term.args[0], duration)) << name << " " << roll;
					}
					if (term.key == "INJURY") {
						ASSERT_FALSE(term.args.empty());
						EXPECT_NE(ParseInjury(term.args[0]), INJ_NONE) << name << " " << roll << ": " << term.args[0];
					}
				}
			}
		}
	}
}

TEST(DCC_Tables, CritSpotChecks)
{
	auto load = [](const char* name) {
		ResultTable results;
		auto table = Open2DA(name);
		if (table) results.Load(*table);
		return results;
	};
	ResultTable one = load("dcccrit1");
	EXPECT_EQ(one.Lookup(10)->effects[0][0].key, "PRONE"); // I-10 prone
	EXPECT_EQ(one.Lookup(14)->effects[0][0].key, "BLIND"); // I-14 blinded 1d3
	EXPECT_EQ(one.Lookup(14)->effects[0][0].args[0], "1d3");
	EXPECT_EQ(one.Lookup(-5)->roll, 0); // anything below 0 is the 0 row
	ResultTable three = load("dcccrit3");
	EXPECT_EQ(three.Lookup(25)->effects[0][0].key, "HALFHP");
	EXPECT_EQ(three.Lookup(11)->effects[0][0].args[0], "1d5H"); // dies in 1d5 hours
	EXPECT_EQ(three.Lookup(40)->roll, 28); // 28+
	ResultTable four = load("dcccrit4");
	EXPECT_EQ(four.Lookup(19)->effects[0][0].key, "WEAPONDMG");
	EXPECT_EQ(four.Lookup(19)->effects[0][0].args[0], "3");
	ResultTable five = load("dcccrit5");
	EXPECT_EQ(five.Lookup(17)->effects[0][0].key, "DIE");
	ResultTable fumbles = load("dccfumbl");
	EXPECT_EQ(fumbles.Lookup(7)->effects[0][0].key, "DISARMSELF");
	EXPECT_EQ(fumbles.Lookup(12)->effects[0][0].key, "HITALLY");
	EXPECT_EQ(fumbles.Lookup(-2)->roll, 0);
	EXPECT_EQ(fumbles.Lookup(20)->roll, 16); // 16+
}

// --- combat completion (M2.3b): the monster tables DR, DN, G and U (crits-fumbles.md), and what
// the crit and fumble tables now express instead of their old TODO notes

TEST(DCC_Tables, CritTableDR)
{
	ExpectDice("dcccritd", 1, { "1d6", "", "", "", "", "2d6", "1d8", "1d12", "", "", "", "1d10", "1d12", "", "2d12", "3d6", "2d6", "1d6", "1d16", "",
				    "2d8", "1d8", "", "1d16", "", "", "", "", "", "" });
}

TEST(DCC_Tables, CritTableDN)
{
	ExpectDice("dcccritn", 1, { "1d8", "", "1d10", "", "", "", "", "", "", "", "", "", "", "", "", "" });
}

TEST(DCC_Tables, CritTableG)
{
	ExpectDice("dcccritg", 1, { "1d8", "1d10", "1d10", "1d12", "1d12", "", "2d6", "1d8", "1d8", "2d8", "", "" });
}

TEST(DCC_Tables, CritTableU)
{
	ExpectDice("dcccritu", 1, { "", "1d4", "1d4", "", "", "", "1d8", "", "", "", "1d10", "", "1d8", "", "", "2d6", "", "", "", "1d6", "", "2d6", "", "1d12", "1d6",
				    "", "1d20", "", "2d12", "" });
}

TEST(DCC_Tables, MonsterCritSaves)
{
	struct Expected {
		const char* table;
		int roll;
		int ability; // -1: a save
		Save save;
		int dc;
		bool plusLevel;
	};
	const Expected saves[] = {
		{ "dcccritd", 4, static_cast<int>(Ability::Str), Save::Fort, 10, true }, // opposed Str vs the dragon's 10 + HD
		{ "dcccritd", 13, -1, Save::Will, 10, true }, { "dcccritd", 19, -1, Save::Fort, 15, true },
		{ "dcccritu", 4, -1, Save::Fort, 15, false }, { "dcccritu", 6, -1, Save::Fort, 10, false }, { "dcccritu", 10, -1, Save::Fort, 16, false },
		{ "dcccritu", 12, -1, Save::Fort, 16, false }, { "dcccritu", 14, -1, Save::Fort, 12, false }, { "dcccritu", 16, -1, Save::Fort, 16, false },
	};
	for (const auto& expected : saves) {
		auto table = Open2DA(expected.table);
		ASSERT_TRUE(table);
		ResultTable results;
		ASSERT_TRUE(results.Load(*table));
		const ResultRow* row = results.Lookup(expected.roll);
		ASSERT_TRUE(row);
		EXPECT_EQ(row->save.ability, expected.ability) << expected.table << " " << expected.roll;
		if (expected.ability < 0) EXPECT_EQ(row->save.save, expected.save) << expected.table << " " << expected.roll;
		EXPECT_EQ(row->save.dc, expected.dc) << expected.table << " " << expected.roll;
		EXPECT_EQ(row->save.plusLevel, expected.plusLevel) << expected.table << " " << expected.roll;
	}
}

static ResultTable LoadResults(const char* name)
{
	ResultTable results;
	auto table = Open2DA(name);
	if (table) results.Load(*table);
	return results;
}

static bool HasKey(const ResultRow* row, const std::string& key, const std::string& arg0 = std::string())
{
	if (!row) return false;
	for (const auto& alternatives : row->effects) {
		for (const auto& term : alternatives) {
			if (term.key == key && (arg0.empty() || (!term.args.empty() && term.args[0] == arg0))) return true;
		}
	}
	return false;
}

TEST(DCC_Tables, CompletedCritsAndFumbles)
{
	ResultTable one = LoadResults("dcccrit1");
	EXPECT_TRUE(HasKey(one.Lookup(6), "EXTRA")); // free attack while it stoops for its weapon
	EXPECT_TRUE(HasKey(one.Lookup(15), "DISARM", "1d10+5")); // knocked 1d10+5' away
	ResultTable two = LoadResults("dcccrit2");
	EXPECT_TRUE(HasKey(two.Lookup(6), "HALFACT"));
	EXPECT_TRUE(HasKey(two.Lookup(16), "INJURY", "EYE"));
	EXPECT_TRUE(HasKey(two.Lookup(17), "ONEACTION"));
	ResultTable three = LoadResults("dcccrit3");
	EXPECT_TRUE(HasKey(three.Lookup(0), "RAGE"));
	EXPECT_TRUE(HasKey(three.Lookup(5), "INJURY", "SMELL"));
	EXPECT_TRUE(HasKey(three.Lookup(7), "DISARM", "1d20+5"));
	EXPECT_TRUE(HasKey(three.Lookup(22), "INJURY", "ARMUSELESS"));
	ResultTable four = LoadResults("dcccrit4");
	EXPECT_TRUE(HasKey(four.Lookup(2), "INJURY", "HAND"));
	EXPECT_TRUE(HasKey(four.Lookup(12), "RAGE"));
	EXPECT_TRUE(HasKey(four.Lookup(18), "INJURY", "AMNESIA"));
	ResultTable five = LoadResults("dcccrit5");
	EXPECT_TRUE(HasKey(five.Lookup(2), "FLING"));
	for (int roll = 8; roll <= 12; ++roll) {
		EXPECT_TRUE(HasKey(five.Lookup(roll), "RAGE")) << roll;
	}
	EXPECT_TRUE(HasKey(five.Lookup(20), "INJURY", "SHIELDARM"));
	EXPECT_TRUE(HasKey(five.Lookup(21), "CLEAVE"));
	EXPECT_TRUE(HasKey(five.Lookup(24), "INJURY", "ARM"));
	EXPECT_TRUE(HasKey(five.Lookup(28), "RAMPAGE"));
	ResultTable monsters = LoadResults("dcccritm");
	EXPECT_TRUE(HasKey(monsters.Lookup(4), "DISARM", "1d12+5"));
	EXPECT_TRUE(HasKey(monsters.Lookup(8), "INJURY", "ARMUSELESS"));
	EXPECT_TRUE(HasKey(monsters.Lookup(30), "RAMPAGE"));
	ResultTable fumbles = LoadResults("dccfumbl");
	EXPECT_TRUE(HasKey(fumbles.Lookup(4), "FOUL", "1T")); // useless for 10 minutes
	EXPECT_TRUE(HasKey(fumbles.Lookup(16), "DOWNED", "16")); // DC 16 Agility check to stand
	// nothing on I-V, M or the fumble table is only a note any more
	for (const char* name : { "dcccrit1", "dcccrit2", "dcccrit3", "dcccrit4", "dcccrit5", "dcccritm", "dccfumbl" }) {
		ResultTable results = LoadResults(name);
		for (int roll = results.MinRoll(); roll <= results.MaxRoll(); ++roll) {
			EXPECT_FALSE(HasKey(results.Lookup(roll), "TODO")) << name << " " << roll;
		}
	}
	// spot checks on the monster tables
	ResultTable dragons = LoadResults("dcccritd");
	EXPECT_TRUE(HasKey(dragons.Lookup(15), "STUCK", "AGI"));
	EXPECT_TRUE(HasKey(dragons.Lookup(27), "DIE"));
	EXPECT_EQ(dragons.Lookup(40)->roll, 30); // 30+
	ResultTable demons = LoadResults("dcccritn");
	EXPECT_TRUE(HasKey(demons.Lookup(2), "MAXHP", "2"));
	EXPECT_TRUE(HasKey(demons.Lookup(16), "DIE", "GONE"));
	ResultTable giants = LoadResults("dcccritg");
	EXPECT_TRUE(HasKey(giants.Lookup(11), "AGAIN", "2"));
	EXPECT_TRUE(HasKey(giants.Lookup(12), "AGAIN", "3"));
	EXPECT_EQ(giants.Lookup(-3)->roll, 1); // 1 or less
	ResultTable undead = LoadResults("dcccritu");
	EXPECT_TRUE(HasKey(undead.Lookup(22), "WOUND", "1d6"));
	EXPECT_TRUE(HasKey(undead.Lookup(27), "DRAIN"));
}

// weapon training by class (classes.md), subdual weapons and backstab damage (equipment.md)
TEST(DCC_Tables, Weapons)
{
	auto table = Open2DA("dccweap");
	ASSERT_TRUE(table);
	WeaponTable weapons;
	ASSERT_TRUE(weapons.Load(*table));
	auto trained = [&weapons](const char* key, WeaponClass which) {
		const WeaponRow* row = weapons.Find(key);
		return row && row->Trained(which);
	};
	struct ClassWeapons {
		WeaponClass which;
		std::vector<const char*> weapons;
	};
	const ClassWeapons lists[] = {
		{ WeaponClass::Wizard, { "DAGGER", "LONGBOW", "LONGSWORD", "SHORTBOW", "SHORT_SWORD", "STAFF" } },
		{ WeaponClass::Thief, { "BLACKJACK", "BLOWGUN", "CROSSBOW", "DAGGER", "DART", "GARROTE", "LONGSWORD", "SHORT_SWORD", "SLING", "STAFF" } },
		{ WeaponClass::Warrior, { "BATTLEAXE", "CLUB", "CROSSBOW", "DAGGER", "DART", "FLAIL", "HANDAXE", "JAVELIN", "LANCE", "LONGBOW", "LONGSWORD", "MACE", "POLEARM",
					  "SHORTBOW", "SHORT_SWORD", "SLING", "SPEAR", "STAFF", "TWO_HANDED_SWORD", "WARHAMMER" } },
		{ WeaponClass::Dwarf, { "BATTLEAXE", "CLUB", "DAGGER", "HANDAXE", "LONGSWORD", "MACE", "SHORT_SWORD", "SPEAR", "TWO_HANDED_SWORD", "WARHAMMER", "CROSSBOW",
					"JAVELIN", "SHORTBOW", "SLING" } },
		{ WeaponClass::Elf, { "DAGGER", "JAVELIN", "LANCE", "LONGBOW", "LONGSWORD", "SHORTBOW", "SHORT_SWORD", "STAFF", "SPEAR", "TWO_HANDED_SWORD" } },
		{ WeaponClass::Halfling, { "CLUB", "CROSSBOW", "DAGGER", "HANDAXE", "JAVELIN", "SHORTBOW", "SHORT_SWORD", "SLING", "STAFF" } },
		{ WeaponClass::ClericLaw, { "CLUB", "MACE", "SLING", "STAFF", "WARHAMMER" } },
		{ WeaponClass::ClericNeutral, { "DAGGER", "MACE", "SLING", "STAFF", "LONGSWORD", "SHORT_SWORD", "TWO_HANDED_SWORD" } },
		{ WeaponClass::ClericChaos, { "BATTLEAXE", "HANDAXE", "LONGBOW", "SHORTBOW", "DAGGER", "DART", "FLAIL" } },
	};
	// the 23 weapons of Table 3-1: trained exactly in the listed ones
	const char* const table31[] = { "BATTLEAXE", "BLACKJACK", "BLOWGUN", "CLUB", "CROSSBOW", "DAGGER", "DART", "FLAIL", "GARROTE", "HANDAXE", "JAVELIN", "LANCE",
					"LONGBOW", "LONGSWORD", "MACE", "POLEARM", "SHORTBOW", "SHORT_SWORD", "SLING", "SPEAR", "STAFF", "TWO_HANDED_SWORD", "WARHAMMER" };
	for (const auto& list : lists) {
		for (const char* weapon : table31) {
			bool listed = std::find_if(list.weapons.begin(), list.weapons.end(), [weapon](const char* w) { return std::string(w) == weapon; }) != list.weapons.end();
			EXPECT_EQ(trained(weapon, list.which), listed) << WeaponClassKey(list.which) << " " << weapon;
		}
		EXPECT_TRUE(trained("UNARMED", list.which));
	}
	// subdual: swords, axes, clubs, spears and staves; the blackjack and fists always
	for (const char* weapon : { "BATTLEAXE", "HANDAXE", "CLUB", "LONGSWORD", "SHORT_SWORD", "TWO_HANDED_SWORD", "SPEAR", "STAFF" }) {
		EXPECT_EQ(weapons.Find(weapon)->subdual, Subdual::Possible) << weapon;
	}
	for (const char* weapon : { "DAGGER", "MACE", "WARHAMMER", "FLAIL", "POLEARM", "LANCE", "JAVELIN", "CROSSBOW", "SHORTBOW", "LONGBOW", "SLING", "DART" }) {
		EXPECT_EQ(weapons.Find(weapon)->subdual, Subdual::Never) << weapon;
	}
	EXPECT_EQ(weapons.Find("BLACKJACK")->subdual, Subdual::Always);
	EXPECT_EQ(weapons.Find("UNARMED")->subdual, Subdual::Always);
	// backstab damage (the second value of Table 3-1)
	auto backstab = [&weapons](const char* key) { return DiceString(weapons.Find(key)->backstab); };
	EXPECT_EQ(backstab("DAGGER"), "1d10");
	EXPECT_EQ(backstab("BLACKJACK"), "2d6");
	EXPECT_EQ(backstab("BLOWGUN"), "1d5");
	EXPECT_EQ(backstab("GARROTE"), "3d4");
	EXPECT_TRUE(weapons.Find("LONGSWORD")->backstab.IsNone());
	// IE items: by item type and hands, resref rows first
	EXPECT_EQ(weapons.Lookup("SW1H01", 20, false)->key, "LONGSWORD");
	EXPECT_EQ(weapons.Lookup("SW2H01", 20, true)->key, "TWO_HANDED_SWORD");
	EXPECT_EQ(weapons.Lookup("AX1H01", 25, false)->key, "HANDAXE");
	EXPECT_EQ(weapons.Lookup("AX1H02", 25, true)->key, "BATTLEAXE");
	EXPECT_EQ(weapons.Lookup("BOW01", 15, true)->key, "SHORTBOW"); // a bow is a shortbow unless named
	EXPECT_EQ(weapons.Lookup("DCCLBOW", 15, true)->key, "LONGBOW");
	EXPECT_EQ(weapons.Lookup("DCCBJACK", 44, false)->key, "BLACKJACK");
	EXPECT_EQ(weapons.Lookup("FIST", 28, false)->key, "UNARMED");
	EXPECT_EQ(weapons.Lookup("WHATEVER", 99, false), nullptr);
}

TEST(DCC_Tables, MonsterCritMatrix)
{
	auto table = Open2DA("dccmcrit");
	ASSERT_TRUE(table);
	MonsterCritTable crits;
	ASSERT_TRUE(crits.Load(*table));
	auto text = [&crits](int hd, MonsterType type) {
		MonsterCrit crit = crits.Lookup(hd, type);
		if (crit.table == CritTableID::None) return std::string("-");
		return std::string(CritTableName(crit.table)) + "/" + (crit.dice > 1 ? std::to_string(crit.dice) : "") + "d" + std::to_string(crit.die);
	};
	const char* humanoid[] = { "III/d4", "III/d6", "III/d8", "III/d8", "III/d10", "III/d10", "IV/d12", "IV/d12", "IV/d14", "IV/d14", "IV/d16",
				   "V/d16", "V/d20", "V/d20", "V/2d10", "V/2d10", "V/2d12", "V/2d12", "V/2d14", "V/2d14", "V/3d10", "V/3d10" };
	const char* other[] = { "M/d4", "M/d6", "M/d8", "M/d8", "M/d10", "M/d10", "M/d12", "M/d12", "M/d14", "M/d14", "M/d16",
				"M/d16", "M/d20", "M/d20", "M/d20", "M/d20", "M/d24", "M/d24", "M/d24", "M/d30", "M/d30", "M/d30" };
	for (int hd = 0; hd <= 21; ++hd) {
		EXPECT_EQ(text(hd, MonsterType::Humanoid), humanoid[hd]) << "HD " << hd;
		EXPECT_EQ(text(hd, MonsterType::Other), other[hd]) << "HD " << hd;
	}
	EXPECT_EQ(text(30, MonsterType::Other), "M/d30"); // 21+
	EXPECT_EQ(text(21, MonsterType::Dragon), "DR/4d20");
	EXPECT_EQ(text(11, MonsterType::Dragon), "DR/2d14");
	EXPECT_EQ(text(0, MonsterType::Demon), "DN/d3");
	EXPECT_EQ(text(3, MonsterType::Giant), "-");
	EXPECT_EQ(text(12, MonsterType::Giant), "G/d6");
	EXPECT_EQ(text(17, MonsterType::Undead), "U/d30");
}

TEST(DCC_Tables, ArmorFumbleDice)
{
	auto table = Open2DA("dccfumdi");
	ASSERT_TRUE(table);
	ArmorFumbleTable dice;
	ASSERT_TRUE(dice.Load(*table));
	// Table 3-3 by IE armor animation: robes none (d4), leather d8, chain d12, plate d16; shields d8
	EXPECT_EQ(FumbleDie(dice.ArmorDie("", "1A"), 0), 4);
	EXPECT_EQ(FumbleDie(dice.ArmorDie("", "2A"), 0), 8);
	EXPECT_EQ(FumbleDie(dice.ArmorDie("", "3A"), 0), 12);
	EXPECT_EQ(FumbleDie(dice.ArmorDie("", "4A"), 0), 16);
	EXPECT_EQ(FumbleDie(0, dice.ShieldDie("")), 8);
	EXPECT_EQ(FumbleDie(dice.ArmorDie("", "3A"), dice.ShieldDie("")), 12);
}

TEST(DCC_Tables, TwoWeaponTable43)
{
	auto table = Open2DA("dcctwowp");
	ASSERT_TRUE(table);
	TwoWeaponTable twoWeapons;
	ASSERT_TRUE(twoWeapons.Load(*table));
	// with a d20: <=8 d12/d10, 9-11 d14/d12, 12-15 d16/d14, 16-17 d16/d16, 18+ d20/d16
	const int bands[][3] = { { 8, 12, 10 }, { 9, 14, 12 }, { 11, 14, 12 }, { 12, 16, 14 }, { 15, 16, 14 }, { 16, 16, 16 }, { 17, 16, 16 }, { 18, 20, 16 }, { 25, 20, 16 } };
	for (const auto& band : bands) {
		TwoWeaponDice dice = twoWeapons.Lookup(band[0], false);
		EXPECT_EQ(AttackDie(20, dice.primarySteps), band[1]) << "Agi " << band[0];
		EXPECT_EQ(AttackDie(20, dice.offSteps), band[2]) << "Agi " << band[0];
	}
	EXPECT_EQ(twoWeapons.Lookup(15, false).primaryCrit, CritRule::None);
	EXPECT_EQ(twoWeapons.Lookup(16, false).primaryCrit, CritRule::MaxOnHit);
	EXPECT_EQ(twoWeapons.Lookup(18, false).primaryCrit, CritRule::Normal);
	EXPECT_EQ(twoWeapons.Lookup(10, true).primaryCrit, CritRule::MaxAutoHit); // halflings
}


// the deed table (combat.md, the seven categories by deed die result): every category has rows
// 3..7, the keys are ones the engine knows, and the rows follow the spec where the engine can
TEST(DCC_Tables, MightyDeeds)
{
	auto file = Open2DA("dccdeeds");
	ASSERT_TRUE(file);
	DeedTable deeds;
	ASSERT_TRUE(deeds.Load(*file));
	static const std::set<std::string> known { "NONE", "PRONE", "NOATTACK", "BLIND", "SPEED", "ATK", "NEXTATK", "DISARM", "SUNDER", "DMGMOD",
						   "PUSH", "GUARD", "ALLYATK", "MORALE", "TODO" };
	auto keys = [](const ResultRow* row) {
		std::vector<std::string> list;
		for (const auto& alternatives : row->effects) {
			for (const auto& term : alternatives) {
				list.push_back(term.key);
			}
		}
		return list;
	};
	for (int i = 1; i < DEED_COUNT; ++i) {
		Deed deed = static_cast<Deed>(i);
		EXPECT_EQ(deeds.Lookup(deed, 2), nullptr) << DeedKey(deed);
		for (int roll = DEED_SUCCESS; roll <= DEED_MAX_RESULT; ++roll) {
			const ResultRow* row = deeds.Lookup(deed, roll);
			ASSERT_TRUE(row) << DeedKey(deed) << roll;
			EXPECT_FALSE(row->text.empty()) << DeedKey(deed) << roll;
			for (const auto& key : keys(row)) {
				EXPECT_TRUE(known.count(key)) << DeedKey(deed) << roll << ": " << key;
			}
		}
	}
	// blinding: -2 then -4 on the next attack, blind 1d4 and 2d6 rounds, a day with a Fort save vs the attack
	EXPECT_EQ(deeds.Lookup(Deed::Blind, 3)->effects[0][0].args[0], "-2");
	EXPECT_EQ(deeds.Lookup(Deed::Blind, 4)->effects[0][0].args[0], "-4");
	EXPECT_EQ(deeds.Lookup(Deed::Blind, 5)->effects[0][0].args[0], "1d4");
	EXPECT_EQ(deeds.Lookup(Deed::Blind, 6)->effects[0][0].args[0], "2d6");
	EXPECT_TRUE(deeds.Lookup(Deed::Blind, 7)->save.vsAttack);
	EXPECT_EQ(deeds.Lookup(Deed::Blind, 7)->save.save, Save::Fort);
	// trips: a Ref save vs the attack roll at 3, automatic from 4
	EXPECT_TRUE(deeds.Lookup(Deed::Trip, 3)->save.vsAttack);
	EXPECT_EQ(deeds.Lookup(Deed::Trip, 3)->save.save, Save::Ref);
	EXPECT_EQ(deeds.Lookup(Deed::Trip, 3)->effects[0][0].gate, '?');
	EXPECT_TRUE(deeds.Lookup(Deed::Trip, 4)->save.IsNone());
	// precision shots: +1d4 up to +1d8
	for (int roll = 3; roll <= 7; ++roll) {
		const DiceExpr& dice = deeds.Lookup(Deed::Precision, roll)->dice;
		EXPECT_EQ(dice.count, 1);
		EXPECT_EQ(dice.sides, roll + 1);
	}
	// defensive maneuvers: +1 alone, +1 with 2 and 4 allies, then +2 and +3
	const int guard[][2] = { { 1, 0 }, { 1, 2 }, { 1, 4 }, { 2, 4 }, { 3, 4 } };
	for (int roll = 3; roll <= 7; ++roll) {
		const TableTerm& term = deeds.Lookup(Deed::Defend, roll)->effects[0][0];
		EXPECT_EQ(term.key, "GUARD");
		EXPECT_EQ(std::stoi(term.args[0]), guard[roll - 3][0]) << roll;
		EXPECT_EQ(std::stoi(term.args[1]), guard[roll - 3][1]) << roll;
	}
}
// --- spellcasting (M2.5) ------------------------------------------------------------

static MagicTableList LoadMagic(const char* name)
{
	MagicTableList list;
	auto table = Open2DA(name);
	EXPECT_TRUE(table);
	if (table) EXPECT_TRUE(list.Load(*table)) << name;
	return list;
}

// Tables 5-3, 5-4, 5-5 (magic.md §8): rows 1-10, the mechanical rows as written
TEST(DCC_Tables, CorruptionTables)
{
	for (const char* name : { "dcccorr1", "dcccorr2", "dcccorr3" }) {
		MagicTableList list = LoadMagic(name);
		ASSERT_EQ(list.Rows().size(), 10u) << name;
		for (int roll = 1; roll <= 10; ++roll) {
			EXPECT_EQ(list.Lookup(roll)->roll, roll) << name;
		}
	}
	MagicTableList minor = LoadMagic("dcccorr1");
	EXPECT_NE(minor.Lookup(1)->effects.find("ABIL:PER:1:PERM"), std::string::npos); // pustules: -1 Per for good
	EXPECT_EQ(minor.Lookup(4)->subDie, 4); // eyes, 1d4
	EXPECT_EQ(minor.Lookup(6)->subDie, 5); // ears, 1d5
	EXPECT_NE(minor.Lookup(10)->effects.find("UNCON:1d6H"), std::string::npos);
	EXPECT_EQ(minor.Lookup(-3)->roll, 1); // 1 or lower
	EXPECT_EQ(minor.Lookup(14)->roll, 10); // 10 or higher
	MagicTableList major = LoadMagic("dcccorr2");
	EXPECT_NE(major.Lookup(1)->effects.find("ABIL:STR:1d4:PERM"), std::string::npos);
	EXPECT_NE(major.Lookup(3)->effects.find("ABIL:STA:1:PERM"), std::string::npos);
	EXPECT_NE(major.Lookup(4)->effects.find("ABIL:AGI:1:PERM"), std::string::npos);
	EXPECT_EQ(major.Lookup(7)->subDie, 3);
	EXPECT_EQ(major.Lookup(8)->subDie, 10); // skin, 1d10 (Ethren: the book's 1d8 lists 10)
	MagicTableList greater = LoadMagic("dcccorr3");
	EXPECT_NE(greater.Lookup(1)->effects.find("DMG:3d6"), std::string::npos);
	EXPECT_NE(greater.Lookup(1)->effects.find("ABIL:LCK:2:PERM"), std::string::npos);
	EXPECT_EQ(greater.Lookup(8)->subDie, 6);
	EXPECT_EQ(greater.Lookup(9)->subDie, 4); // Ethren: the book's 1d6 lists 4
}

// Table 5-6 (generic misfire, d8), Table 5-7 (disapproval, 1-20), Table 5-1 (spellburn, d24)
TEST(DCC_Tables, MisfireDisapprovalSpellburn)
{
	MagicTableList misfire = LoadMagic("dccmisf");
	ASSERT_EQ(misfire.Rows().size(), 8u);
	EXPECT_NE(misfire.Lookup(4)->effects.find("DMG:1d3xSL^NEAREST"), std::string::npos);
	EXPECT_NE(misfire.Lookup(6)->effects.find("MINORROLL:1d12+5"), std::string::npos);
	MagicTableList disapproval = LoadMagic("dccdisap");
	ASSERT_EQ(disapproval.Rows().size(), 20u);
	EXPECT_EQ(disapproval.Lookup(4)->effects, "SPELLMOD:-1:HEAL");
	EXPECT_EQ(disapproval.Lookup(9)->effects, "SPELLMOD:-2:HEAL");
	EXPECT_EQ(disapproval.Lookup(10)->effects, "LOSESPELLS:1:1");
	EXPECT_EQ(disapproval.Lookup(13)->effects, "LOSESPELLS:2:1");
	EXPECT_EQ(disapproval.Lookup(15)->effects, "NORESET");
	EXPECT_EQ(disapproval.Lookup(16)->effects, "BLOCK:LOH:1d4D");
	EXPECT_EQ(disapproval.Lookup(18)->effects, "BLOCK:TURN:1d4D");
	EXPECT_EQ(disapproval.Lookup(25)->roll, 20); // 20 and up
	MagicTableList burn = LoadMagic("dccburn");
	EXPECT_EQ(burn.Rows().size(), 24u);
}

// mercurial magic: every d% result, 41-60 one row
TEST(DCC_Tables, Mercurial)
{
	MagicTableList mercurial = LoadMagic("dccmerc");
	EXPECT_EQ(mercurial.Rows().size(), 81u);
	for (int roll = 1; roll <= 100; ++roll) {
		const MagicEntry* entry = mercurial.Lookup(roll);
		ASSERT_TRUE(entry);
		if (roll >= 41 && roll <= 60) {
			EXPECT_EQ(entry->roll, 41);
		} else {
			EXPECT_EQ(entry->roll, roll);
		}
	}
	EXPECT_EQ(mercurial.Lookup(4)->effects, "ABIL:STA:1d3");
	EXPECT_EQ(mercurial.Lookup(12)->effects, "SPELLMOD:-4:1d4");
	EXPECT_EQ(mercurial.Lookup(24)->subDie, 6);
	EXPECT_EQ(mercurial.Lookup(32)->subDie, 10);
	EXPECT_EQ(mercurial.Lookup(84)->subDie, 6);
}

// Table 4-4 (combat.md "Turn unholy")
TEST(DCC_Tables, TurnUnholy)
{
	auto table = Open2DA("dccturn");
	ASSERT_TRUE(table);
	MagicTables magic;
	ASSERT_TRUE(magic.LoadTurn(*table));
	ASSERT_EQ(magic.turn.size(), 9u);
	EXPECT_EQ(magic.TurnFor(11)->min, 1);
	EXPECT_EQ(magic.TurnFor(11)->cells[0].kind, TurnCell::Kind::None);
	const TurnRow* row = magic.TurnFor(18);
	EXPECT_EQ(row->range, 30);
	EXPECT_EQ(row->cells[0].sides, 4);
	EXPECT_TRUE(row->cells[0].addCL);
	EXPECT_EQ(row->cells[1].kind, TurnCell::Kind::Turn);
	EXPECT_EQ(row->cells[2].kind, TurnCell::Kind::None);
	row = magic.TurnFor(25);
	EXPECT_EQ(row->smiteDice, 1);
	EXPECT_EQ(row->smiteSides, 3);
	EXPECT_TRUE(row->cells[0].noSave);
	EXPECT_EQ(row->cells[0].damageSides, 4);
	row = magic.TurnFor(31);
	EXPECT_EQ(row->range, 120);
	EXPECT_EQ(row->cells[0].kind, TurnCell::Kind::Kill);
	row = magic.TurnFor(40);
	EXPECT_EQ(row->range, 240);
	EXPECT_EQ(row->cells[6].kind, TurnCell::Kind::Turn);
	EXPECT_EQ(row->cells[7].kind, TurnCell::Kind::Turn); // past 12 HD: as 11-12
	EXPECT_FALSE(row->cells[2].noSave); // 3-4 HD at 32+: a save (the spec's open question)
}

// Table 3-3's armor check penalties beside the fumble dice
TEST(DCC_Tables, ArmorCheckPenalty)
{
	auto table = Open2DA("dccfumdi");
	ASSERT_TRUE(table);
	ArmorFumbleTable armor;
	ASSERT_TRUE(armor.Load(*table));
	EXPECT_EQ(armor.ArmorCheck("", "1A"), 0);
	EXPECT_EQ(armor.ArmorCheck("", "2A"), -1);
	EXPECT_EQ(armor.ArmorCheck("", "3A"), -5);
	EXPECT_EQ(armor.ArmorCheck("", "4A"), -8);
	EXPECT_EQ(armor.ShieldCheck("anyshield"), -1);
	EXPECT_EQ(armor.ArmorDie("", "3A"), 12); // the fumble dice stay
}

}
}

namespace GemRB {
namespace DCC {

// Phase 3: Tables 5-8 and 5-9 (spell-format.md), with the starter set's content, and Table 1-5's
// spells known (magic.md)
TEST(DCC_Tables, SpellListsAndClericSpellsKnown)
{
	auto table = Open2DA("dccspls");
	ASSERT_TRUE(table);
	SpellLists lists;
	ASSERT_TRUE(lists.Load(*table));
	const int wizardRows[] = { 27, 24, 24, 6, 5 };
	const int clericRows[] = { 11, 11, 7, 4, 3 };
	for (int level = 1; level <= SPELL_LEVELS; ++level) {
		EXPECT_EQ(lists.List(CasterKind::Wizard, level).size(), size_t(wizardRows[level - 1])) << "Table 5-8 level " << level;
		EXPECT_EQ(lists.List(CasterKind::Cleric, level).size(), size_t(clericRows[level - 1])) << "Table 5-9 level " << level;
		int row = 1;
		for (const auto& entry : lists.List(CasterKind::Wizard, level)) {
			EXPECT_EQ(entry.row, row++);
		}
	}
	const auto& w1 = lists.List(CasterKind::Wizard, 1);
	EXPECT_EQ(w1[12].name, "Flaming hands");
	EXPECT_EQ(w1[12].spell, "dwflam");
	EXPECT_EQ(w1[15].spell, "dwmmis");
	EXPECT_EQ(w1[22].spell, "dwslee");
	EXPECT_EQ(w1[14].flags, LIST_PATRON_PAIR); // invoke patron
	EXPECT_EQ(w1[18].flags, LIST_PATRON_PAIR); // patron bond
	EXPECT_EQ(w1[26].flags, LIST_PATRON_SPELL);
	EXPECT_EQ(w1[7].flags, LIST_CLERIC_SPELL); // detect magic
	EXPECT_EQ(lists.List(CasterKind::Wizard, 3)[14].spell, "dcpara"); // paralysis, a cleric spell
	EXPECT_EQ(lists.List(CasterKind::Wizard, 3)[14].flags, LIST_CLERIC_SPELL);
	const auto& c1 = lists.List(CasterKind::Cleric, 1);
	EXPECT_EQ(c1[0].spell, "dcbles");
	EXPECT_EQ(c1[6].spell, "dcpara");
	EXPECT_EQ(c1[10].spell, "dcword");
	EXPECT_EQ(c1[10].name, "Word of command");
	int content = 0;
	for (CasterKind kind : { CasterKind::Wizard, CasterKind::Cleric }) {
		for (int level = 1; level <= SPELL_LEVELS; ++level) {
			for (const auto& entry : lists.List(kind, level)) {
				content += entry.spell.empty() ? 0 : 1;
			}
		}
	}
	EXPECT_EQ(content, 7); // the six starter spells, paralysis on both lists

	table = Open2DA("dccclesp");
	ASSERT_TRUE(table);
	ClericSpellsTable cleric;
	ASSERT_TRUE(cleric.Load(*table));
	const int expected[10][5] = { { 4, 0, 0, 0, 0 }, { 5, 0, 0, 0, 0 }, { 5, 3, 0, 0, 0 }, { 6, 4, 0, 0, 0 }, { 6, 5, 2, 0, 0 },
				      { 7, 5, 3, 0, 0 }, { 7, 6, 4, 1, 0 }, { 8, 6, 5, 2, 0 }, { 8, 7, 5, 3, 1 }, { 9, 7, 6, 4, 2 } };
	for (int level = 1; level <= 10; ++level) {
		for (int spellLevel = 1; spellLevel <= SPELL_LEVELS; ++spellLevel) {
			EXPECT_EQ(cleric.Known(level, spellLevel), expected[level - 1][spellLevel - 1]) << "cleric " << level << " L" << spellLevel;
		}
	}
}

// M2.10: body weight by race and the load limits (dccbody.2da)
TEST(DCC_Tables, BodyWeight)
{
	auto table = Open2DA("dccbody");
	ASSERT_TRUE(table);
	BodyTable body;
	ASSERT_TRUE(body.Load(*table));
	EXPECT_EQ(body.Average(1), 137);
	EXPECT_EQ(body.Average(2), 102);
	EXPECT_EQ(body.Average(4), 142);
	EXPECT_EQ(body.Average(5), 56);
	// the book: no halfling over 70 lb, nobody carries more than half the body weight
	EXPECT_EQ(body.Roll(5, [](int sides) { return sides; }), 70);
	for (int race : { 1, 2, 4, 5 }) {
		EXPECT_EQ(body.Row(race).capPercent, 50) << race;
		EXPECT_EQ(body.Row(race).halfPercent, 33) << race;
	}
}

// M2.11: the hair and beard overlays (dcchair.2da), the walk speeds (moverate.2da) and the avatars
TEST(DCC_Tables, Looks)
{
	auto table = Open2DA("dcchair");
	ASSERT_TRUE(table);
	HairTable hair;
	ASSERT_TRUE(hair.Load(*table));
	const HairStyle* parted = hair.Find(1, false);
	ASSERT_NE(parted, nullptr);
	EXPECT_EQ(HairOverlay(parted, false), "HP");
	EXPECT_EQ(HairOverlay(parted, true), ""); // hidden under a helmet
	const HairStyle* beard = hair.Find(1, true);
	ASSERT_NE(beard, nullptr);
	EXPECT_EQ(HairOverlay(beard, true), "BD"); // a beard shows under a helmet
	EXPECT_EQ(hair.Find(0, false), nullptr);

	auto rates = Open2DA("moverate");
	ASSERT_TRUE(rates);
	for (TableMgr::index_t row = 0; row < rates->GetRowCount(); ++row) {
		EXPECT_EQ(rates->QueryFieldSigned<int>(row, 1), 9) << rates->QueryField(row, 0);
	}
	auto avatars = Open2DA("avatars");
	ASSERT_TRUE(avatars);
	std::set<std::string> ids;
	for (TableMgr::index_t row = 0; row < avatars->GetRowCount(); ++row) {
		ids.insert(avatars->GetRowName(row));
		EXPECT_EQ(avatars->QueryField(row, 4), "0"); // TYPE 0, IE_ANI_CODE_MIRROR
		EXPECT_EQ(avatars->QueryField(row, 7), "L"); // SIZE L: overlays on
	}
	EXPECT_EQ(ids, (std::set<std::string> { "0x6500", "0x6501", "0x6510" }));
}

// Phase 3: the formations for parties of 16 (formatio.2da): 20 places each, none shared
TEST(DCC_Tables, Formations)
{
	auto table = Open2DA("formatio");
	ASSERT_TRUE(table);
	EXPECT_EQ(table->GetColumnCount(), 40U);
	ASSERT_GE(table->GetRowCount(), 1U);
	EXPECT_EQ(table->GetRowName(0), "BLOCK");
	for (TableMgr::index_t row = 0; row < table->GetRowCount(); ++row) {
		std::set<std::pair<int, int>> places;
		for (TableMgr::index_t i = 0; i < 16; ++i) {
			places.emplace(table->QueryFieldSigned<int>(row, i * 2), table->QueryFieldSigned<int>(row, i * 2 + 1));
		}
		EXPECT_EQ(places.size(), 16U) << table->GetRowName(row);
	}
}

}
}
