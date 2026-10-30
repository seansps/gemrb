// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Classes.h"

#include "globals.h"

#include <algorithm>

namespace GemRB {
namespace DCC {

bool XPTable::SetThresholds(std::vector<int> newThresholds)
{
	thresholds.clear();
	if (newThresholds.empty() || newThresholds[0] != 0) return false;
	for (size_t i = 1; i < newThresholds.size(); ++i) {
		if (newThresholds[i] <= newThresholds[i - 1]) return false;
	}
	thresholds = std::move(newThresholds);
	return true;
}

bool XPTable::Load(const TableMgr& table)
{
	thresholds.clear();
	auto xpCol = table.GetColumnIndex("XP");
	if (xpCol == TableMgr::npos) return false;

	std::vector<int> newThresholds(table.GetRowCount(), -1);
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		int level = -1;
		if (!valid_signednumber(table.GetRowName(row).c_str(), level)) return false;
		if (level < 0 || level >= int(newThresholds.size()) || newThresholds[level] != -1) return false;
		newThresholds[level] = table.QueryFieldSigned<int>(row, xpCol);
	}
	return SetThresholds(std::move(newThresholds));
}

int XPTable::XPForLevel(int level) const
{
	if (level < 0 || level > MaxLevel()) return -1;
	return thresholds[level];
}

int XPTable::LevelForXP(int xp) const
{
	int level = 0;
	for (int i = 0; i <= MaxLevel(); ++i) {
		if (xp >= thresholds[i]) level = i;
	}
	return level;
}

int LevelRow::SaveBonus(Save save) const
{
	switch (save) {
		case Save::Fort:
			return fort;
		case Save::Ref:
			return ref;
		default:
			return will;
	}
}

static bool KnownClassID(int id)
{
	return id >= static_cast<int>(ClassID::Wizard) && id <= static_cast<int>(ClassID::Level0);
}

bool ClassTable::SetClasses(std::vector<ClassInfo> newClasses)
{
	classes.clear();
	bool haveLevel0 = false;
	for (const ClassInfo& info : newClasses) {
		if (!KnownClassID(info.id) || info.hitDie <= 0 || info.levels.empty()) return false;
		if (std::count_if(newClasses.begin(), newClasses.end(), [&info](const ClassInfo& other) { return other.id == info.id; }) != 1) return false;
		if (info.id == static_cast<int>(ClassID::Level0)) {
			// the level 0 table has exactly level 0
			if (info.levels.size() != 1) return false;
			haveLevel0 = true;
		} else if (info.levels.size() < 2) {
			return false;
		}
	}
	if (!haveLevel0) return false;

	classes = std::move(newClasses);
	return true;
}

bool ClassTable::ReadClassRow(const TableMgr& table, TableMgr::index_t row, ClassInfo& info) const
{
	auto idCol = table.GetColumnIndex("ID");
	auto hdCol = table.GetColumnIndex("HIT_DIE");
	auto deedCol = table.GetColumnIndex("DEED");
	auto raceCol = table.GetColumnIndex("RACE");
	auto nameCol = table.GetColumnIndex("NAME");
	if (idCol == TableMgr::npos || hdCol == TableMgr::npos || deedCol == TableMgr::npos ||
	    raceCol == TableMgr::npos || nameCol == TableMgr::npos || table.GetColumnIndex("TABLE") == TableMgr::npos) {
		return false;
	}
	info.id = table.QueryFieldSigned<int>(row, idCol);
	info.hitDie = table.QueryFieldSigned<int>(row, hdCol);
	info.deed = table.QueryFieldSigned<int>(row, deedCol) != 0;
	info.race = table.QueryFieldSigned<int>(row, raceCol);
	info.name = table.QueryField(row, nameCol);
	return true;
}

bool ClassTable::LoadProgression(const TableMgr& table, ClassInfo& info)
{
	// columns in LevelRow order
	static const char* columns[] = {
		"ATTACK", "DEED_DIE", "DEED_ADD", "CRIT_DICE", "CRIT_DIE", "CRIT_ADD", "CRIT_TABLE", "THREAT",
		"ACT1", "ACT2", "ACT3", "REF", "FORT", "WILL", "LUCK_DIE", "SNEAK_HIDE", "SPELLS_KNOWN", "MAX_SPELL_LVL"
	};
	std::array<TableMgr::index_t, sizeof(columns) / sizeof(columns[0])> idx;
	for (size_t i = 0; i < idx.size(); ++i) {
		idx[i] = table.GetColumnIndex(StringView(columns[i]));
		if (idx[i] == TableMgr::npos) return false;
	}

	bool level0 = info.id == static_cast<int>(ClassID::Level0);
	info.levels.clear();
	info.levels.resize(table.GetRowCount() + (level0 ? 0 : 1));
	std::vector<bool> seen(info.levels.size(), false);
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		int level = -1;
		if (!valid_signednumber(table.GetRowName(row).c_str(), level)) return false;
		if (level < 0 || level >= int(info.levels.size()) || seen[level]) return false;
		if (level0 != (level == 0)) return false;
		seen[level] = true;

		auto field = [&table, row, &idx](size_t i) { return table.QueryFieldSigned<int>(row, idx[i]); };
		LevelRow& entry = info.levels[level];
		entry.attack = field(0);
		entry.deedDie = field(1);
		entry.deedAdd = field(2);
		entry.critDice = field(3);
		entry.critDie = field(4);
		entry.critAdd = field(5);
		entry.critTable = field(6);
		entry.threat = field(7);
		entry.actionDice = { field(8), field(9), field(10) };
		entry.ref = field(11);
		entry.fort = field(12);
		entry.will = field(13);
		entry.luckDie = field(14);
		entry.sneakHide = field(15);
		entry.spellsKnown = field(16);
		entry.maxSpellLevel = field(17);
	}
	// every level from 1 (0 for LEVEL0) up must be there
	for (size_t level = level0 ? 0 : 1; level < seen.size(); ++level) {
		if (!seen[level]) return false;
	}
	return true;
}

const ClassInfo* ClassTable::Class(int id) const
{
	for (const ClassInfo& info : classes) {
		if (info.id == id) return &info;
	}
	return nullptr;
}

const LevelRow* ClassTable::Row(int id, int level) const
{
	const ClassInfo* info = Class(id);
	if (!info) return nullptr;
	if (level <= 0 || id == static_cast<int>(ClassID::Level0)) {
		const ClassInfo* level0 = Class(static_cast<int>(ClassID::Level0));
		return level0 ? &level0->levels[0] : nullptr;
	}
	level = std::min(level, int(info->levels.size()) - 1);
	return &info->levels[level];
}

int ClassTable::HitDie(int id, int level) const
{
	const ClassInfo* info = Class(level <= 0 ? static_cast<int>(ClassID::Level0) : id);
	return info ? info->hitDie : 0;
}

static const std::array<const char*, THIEF_SKILL_COUNT> thiefSkillKeys {
	"BACKSTAB", "SNEAK", "HIDE", "PICK_POCKET", "CLIMB", "PICK_LOCK", "FIND_TRAP",
	"DISABLE_TRAP", "FORGE", "DISGUISE", "READ_LANG", "HANDLE_POISON", "SCROLL_DIE"
};

const char* ThiefSkillKey(ThiefSkill skill)
{
	size_t i = static_cast<size_t>(skill);
	return i < thiefSkillKeys.size() ? thiefSkillKeys[i] : "";
}

bool ThiefSkillAbility(ThiefSkill skill, Ability& ability)
{
	switch (skill) {
		case ThiefSkill::Sneak:
		case ThiefSkill::Hide:
		case ThiefSkill::PickPocket:
		case ThiefSkill::Climb:
		case ThiefSkill::PickLock:
		case ThiefSkill::DisableTrap:
		case ThiefSkill::Forge:
			ability = Ability::Agi;
			return true;
		case ThiefSkill::FindTrap:
		case ThiefSkill::ReadLanguages:
		case ThiefSkill::ScrollDie:
			ability = Ability::Int;
			return true;
		case ThiefSkill::Disguise:
			ability = Ability::Per;
			return true;
		default:
			return false;
	}
}

bool ThiefSkillTable::SetRows(Alignment path, std::vector<Row> rows)
{
	size_t i = static_cast<size_t>(path);
	if (i >= paths.size()) return false;
	paths[i] = std::move(rows);
	return !paths[i].empty();
}

bool ThiefSkillTable::Load(Alignment path, const TableMgr& table)
{
	std::array<TableMgr::index_t, THIEF_SKILL_COUNT> idx;
	for (size_t i = 0; i < idx.size(); ++i) {
		idx[i] = table.GetColumnIndex(StringView(thiefSkillKeys[i]));
		if (idx[i] == TableMgr::npos) return false;
	}

	std::vector<Row> rows(table.GetRowCount());
	std::vector<bool> seen(rows.size(), false);
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		int level = 0;
		if (!valid_signednumber(table.GetRowName(row).c_str(), level)) return false;
		if (level < 1 || level > int(rows.size()) || seen[level - 1]) return false;
		seen[level - 1] = true;
		for (size_t i = 0; i < idx.size(); ++i) {
			rows[level - 1][i] = table.QueryFieldSigned<int>(row, idx[i]);
		}
	}
	return SetRows(path, std::move(rows));
}

bool ThiefSkillTable::IsEmpty() const
{
	return std::any_of(paths.begin(), paths.end(), [](const std::vector<Row>& rows) { return rows.empty(); });
}

int ThiefSkillTable::Bonus(Alignment path, int level, ThiefSkill skill) const
{
	const std::vector<Row>& rows = paths[static_cast<size_t>(path)];
	if (rows.empty() || level < 1) return 0;
	level = std::min(level, int(rows.size()));
	return rows[level - 1][static_cast<size_t>(skill)];
}

}
}
