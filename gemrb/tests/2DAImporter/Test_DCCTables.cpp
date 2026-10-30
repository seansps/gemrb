// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Checks the shipped DCC tables in unhardcoded/ethren against the spec
// (docs/rules/classes.md Tables 1-4 to 1-18, luck.md Table 1-2, funnel.md),
// read through the real 2DA importer and the DCC loaders.

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
						   "SELFHIT", "HITALLY", "DELAYDMG", "EXTRA", "TODO" };
	static const std::set<std::string> conditions { "", "NOHELM", "NOSHIELD", "ARMORED", "HASWEAPON" };
	for (const char* name : { "dcccrit1", "dcccrit2", "dcccrit3", "dcccrit4", "dcccrit5", "dcccritm", "dccfumbl" }) {
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

}
}
