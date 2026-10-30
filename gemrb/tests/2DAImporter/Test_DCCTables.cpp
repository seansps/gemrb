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

}
}
