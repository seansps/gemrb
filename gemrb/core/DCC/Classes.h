// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Classes.h
 * DCC classes and levels (docs/rules/classes.md, funnel.md): the XP
 * thresholds (Table 1-4), the class list with hit dice, and one progression
 * table per class with attack bonus or deed die, crit die and table, action
 * dice, saves, the thief luck die, halfling sneak & hide and spells known.
 * The thief skills (Table 1-9) have a table per alignment path.
 *
 * Data, all in unhardcoded/ethren:
 * - dccxp.2da: rows 0-10, column XP
 * - dccclass.2da: a row per class with ID (IE_CLASS), HIT_DIE, DEED, RACE
 *   (the IE race the class needs, 0 any), TABLE (the progression 2DA) and NAME
 * - dccl*.2da: progression, rows by level (1-10; the level 0 table has row 0)
 * - dccthl/dccthc/dccthn.2da: thief skills of the lawful, chaotic and
 *   neutral paths, rows 1-10
 *
 * A character of level 0 uses the LEVEL0 class row, whatever its class.
 */

#ifndef DCC_CLASSES_H
#define DCC_CLASSES_H

#include "exports.h"

#include "DCC/Character.h"
#include "TableMgr.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace GemRB {
namespace DCC {

// IE_CLASS values of the DCC classes (class.ids of the ethren game type)
enum class ClassID : uint8_t {
	None = 0, // monsters and anything that isn't a DCC character
	Wizard = 1,
	Warrior = 2,
	Cleric = 3,
	Thief = 4,
	Dwarf = 5,
	Elf = 6,
	Halfling = 7,
	Level0 = 8 // a 0-level character (peasant), before choosing a class
};
constexpr int MAX_LEVEL = 10;

class GEM_EXPORT XPTable {
public:
	/** thresholds[level] for levels 0..n; must start at 0 and rise */
	bool SetThresholds(std::vector<int> newThresholds);
	/** reads a 2DA with level row names 0..n and an XP column */
	bool Load(const TableMgr& table);

	bool IsEmpty() const { return thresholds.empty(); }
	int MaxLevel() const { return static_cast<int>(thresholds.size()) - 1; }
	/** the XP a level needs, -1 past the table */
	int XPForLevel(int level) const;
	/** the highest level xp reaches */
	int LevelForXP(int xp) const;

private:
	std::vector<int> thresholds;
};

struct LevelRow {
	int attack = 0; // fixed attack bonus (classes without a deed die)
	int deedDie = 0; // warriors and dwarves: deed die sides, plus deedAdd from level 7
	int deedAdd = 0;
	int critDice = 1; // crit roll: critDice d critDie + critAdd on table critTable (1-5: I-V)
	int critDie = 4;
	int critAdd = 0;
	int critTable = 1;
	int threat = 20; // lowest natural roll that threatens a crit
	std::array<int, 3> actionDice {}; // die sides, 0: no such die
	int ref = 0;
	int fort = 0;
	int will = 0;
	int luckDie = 0; // thieves
	int sneakHide = 0; // halflings
	int spellsKnown = 0; // wizards and elves
	int maxSpellLevel = 0;

	int SaveBonus(Save save) const;
};

struct ClassInfo {
	int id = 0;
	std::string name;
	int hitDie = 0;
	bool deed = false;
	int race = 0; // IE race needed to take the class, 0: any
	std::vector<LevelRow> levels; // levels[i] is level i; levels[0] only for the LEVEL0 class
};

class GEM_EXPORT ClassTable {
public:
	/** checks the classes (known ids, a LEVEL0 class with a level 0 row, the others with rows 1..n) */
	bool SetClasses(std::vector<ClassInfo> newClasses);
	/** reads dccclass.2da and, through loadTable (name -> table pointer), each class's progression table */
	template<typename LOADER>
	bool Load(const TableMgr& classList, LOADER&& loadTable);
	/** reads one progression table (rows by level) into info.levels */
	static bool LoadProgression(const TableMgr& table, ClassInfo& info);

	bool IsEmpty() const { return classes.empty(); }
	const ClassInfo* Class(int id) const;
	/** true for the DCC character classes (LEVEL0 included) */
	bool IsCharacterClass(int id) const { return Class(id) != nullptr; }
	/** the row for a level; level 0 is the LEVEL0 row for every class, levels past the table clamp */
	const LevelRow* Row(int id, int level) const;
	/** the hit die a character of this class rolls for a level (level 0: the LEVEL0 die) */
	int HitDie(int id, int level) const;

private:
	bool ReadClassRow(const TableMgr& table, TableMgr::index_t row, ClassInfo& info) const;
	std::vector<ClassInfo> classes;
};

// Table 1-9 skills; the scroll entry is the die for casting from scrolls
enum class ThiefSkill : uint8_t {
	Backstab,
	Sneak,
	Hide,
	PickPocket,
	Climb,
	PickLock,
	FindTrap,
	DisableTrap,
	Forge,
	Disguise,
	ReadLanguages,
	HandlePoison,
	ScrollDie,
	count
};
constexpr int THIEF_SKILL_COUNT = static_cast<int>(ThiefSkill::count);

/** the 2DA column of a skill (BACKSTAB, SNEAK, ...) */
GEM_EXPORT const char* ThiefSkillKey(ThiefSkill skill);
/** the ability whose modifier a skill adds; false for backstab and handle poison */
GEM_EXPORT bool ThiefSkillAbility(ThiefSkill skill, Ability& ability);

class GEM_EXPORT ThiefSkillTable {
public:
	using Row = std::array<int, THIEF_SKILL_COUNT>;
	/** rows[level - 1] per path (lawful, chaotic, neutral) */
	bool SetRows(Alignment path, std::vector<Row> rows);
	/** reads one path's 2DA: level row names 1..n, a column per skill */
	bool Load(Alignment path, const TableMgr& table);

	bool IsEmpty() const;
	/** the table value for a level (clamped to the table); 0 if missing */
	int Bonus(Alignment path, int level, ThiefSkill skill) const;

private:
	std::array<std::vector<Row>, 3> paths;
};

template<typename LOADER>
bool ClassTable::Load(const TableMgr& classList, LOADER&& loadTable)
{
	classes.clear();
	std::vector<ClassInfo> newClasses;
	for (TableMgr::index_t row = 0; row < classList.GetRowCount(); ++row) {
		ClassInfo info;
		if (!ReadClassRow(classList, row, info)) return false;
		// a smart pointer or anything else that dereferences to a TableMgr
		auto progression = loadTable(classList.QueryField(row, classList.GetColumnIndex("TABLE")));
		if (!progression || !LoadProgression(*progression, info)) return false;
		newClasses.push_back(std::move(info));
	}
	return SetClasses(std::move(newClasses));
}

}
}

#endif
