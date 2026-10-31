// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Learning.h"

#include "TableMgr.h"

#include <algorithm>
#include <cctype>
#include <fmt/format.h>

namespace GemRB {
namespace DCC {

namespace {

bool ParseNumber(const std::string& text, int& value)
{
	if (text.empty()) return false;
	for (char c : text) {
		if (!std::isdigit(static_cast<unsigned char>(c))) return false;
	}
	value = std::stoi(text);
	return true;
}

std::string Lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return text;
}

int ListIndex(CasterKind kind)
{
	return kind == CasterKind::Cleric ? 1 : 0;
}

const std::vector<SpellListEntry>& NoList()
{
	static const std::vector<SpellListEntry> none;
	return none;
}

// a pick from the spells of a level that have content, aren't known and weren't picked yet
std::string Substitute(const std::vector<SpellListEntry>& list, const std::set<std::string>& taken,
		       const std::function<bool(const std::string&)>& available, const Roller& roller)
{
	std::vector<std::string> pool;
	for (const auto& entry : list) {
		if (entry.spell.empty() || taken.count(entry.spell) || !available(entry.spell)) continue;
		if (std::find(pool.begin(), pool.end(), entry.spell) == pool.end()) pool.push_back(entry.spell);
	}
	if (pool.empty()) return {};
	int pick = pool.size() == 1 ? 1 : roller(int(pool.size()));
	return pool[size_t(std::min(std::max(pick, 1), int(pool.size())) - 1)];
}

const char* ListName(CasterKind kind)
{
	return kind == CasterKind::Cleric ? "Table 5-9" : "Table 5-8";
}

}

bool SpellLists::Load(const TableMgr& table)
{
	for (auto& kind : lists) {
		for (auto& level : kind) {
			level.clear();
		}
	}
	auto levelCol = table.GetColumnIndex("LEVEL");
	auto rowCol = table.GetColumnIndex("ROW");
	auto spellCol = table.GetColumnIndex("SPELL");
	auto flagsCol = table.GetColumnIndex("FLAGS");
	auto nameCol = table.GetColumnIndex("NAME");
	if (levelCol == TableMgr::npos || rowCol == TableMgr::npos || spellCol == TableMgr::npos || nameCol == TableMgr::npos) return false;
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		const std::string& key = table.GetRowName(row);
		if (key.empty()) return false;
		CasterKind kind = std::toupper(static_cast<unsigned char>(key[0])) == 'C' ? CasterKind::Cleric : CasterKind::Wizard;
		SpellListEntry entry;
		if (!ParseNumber(table.QueryField(row, levelCol), entry.level) || !ParseNumber(table.QueryField(row, rowCol), entry.row)) return false;
		if (entry.level < 1 || entry.level > SPELL_LEVELS) return false;
		std::string spell = table.QueryField(row, spellCol);
		entry.spell = spell == "*" ? std::string() : Lower(spell);
		std::string flags = flagsCol == TableMgr::npos ? std::string("*") : table.QueryField(row, flagsCol);
		if (flags == "CLERIC") entry.flags = LIST_CLERIC_SPELL;
		else if (flags == "PAIR") entry.flags = LIST_PATRON_PAIR;
		else if (flags == "PATRON") entry.flags = LIST_PATRON_SPELL;
		else if (flags != "*") return false;
		entry.name = table.QueryField(row, nameCol);
		std::replace(entry.name.begin(), entry.name.end(), '_', ' ');
		lists[size_t(ListIndex(kind))][size_t(entry.level - 1)].push_back(std::move(entry));
	}
	for (auto& kind : lists) {
		for (auto& level : kind) {
			std::stable_sort(level.begin(), level.end(), [](const SpellListEntry& a, const SpellListEntry& b) { return a.row < b.row; });
		}
	}
	return !IsEmpty();
}

const std::vector<SpellListEntry>& SpellLists::List(CasterKind kind, int level) const
{
	if (kind == CasterKind::None || level < 1 || level > SPELL_LEVELS) return NoList();
	return lists[size_t(ListIndex(kind))][size_t(level - 1)];
}

const SpellListEntry* SpellLists::Find(CasterKind kind, const std::string& spell) const
{
	if (kind == CasterKind::None || spell.empty()) return nullptr;
	std::string name = Lower(spell);
	for (const auto& level : lists[size_t(ListIndex(kind))]) {
		for (const auto& entry : level) {
			if (entry.spell == name) return &entry;
		}
	}
	return nullptr;
}

bool SpellLists::IsEmpty() const
{
	for (const auto& kind : lists) {
		for (const auto& level : kind) {
			if (!level.empty()) return false;
		}
	}
	return true;
}

void SpellLists::Set(CasterKind kind, int level, std::vector<SpellListEntry> list)
{
	if (kind == CasterKind::None || level < 1 || level > SPELL_LEVELS) return;
	lists[size_t(ListIndex(kind))][size_t(level - 1)] = std::move(list);
}

bool ClericSpellsTable::Load(const TableMgr& table)
{
	rows.clear();
	std::array<TableMgr::index_t, SPELL_LEVELS> cols {};
	for (int i = 0; i < SPELL_LEVELS; ++i) {
		cols[size_t(i)] = table.GetColumnIndex(fmt::format("L{}", i + 1));
		if (cols[size_t(i)] == TableMgr::npos) return false;
	}
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		int level = 0;
		if (!ParseNumber(table.GetRowName(row), level) || level != int(row) + 1) return false;
		std::array<int, SPELL_LEVELS> counts {};
		for (int i = 0; i < SPELL_LEVELS; ++i) {
			const std::string& cell = table.QueryField(row, cols[size_t(i)]);
			if (cell != "*" && !ParseNumber(cell, counts[size_t(i)])) return false;
		}
		rows.push_back(counts);
	}
	return !rows.empty();
}

void ClericSpellsTable::Set(int level, std::array<int, SPELL_LEVELS> counts)
{
	if (level < 1) return;
	if (rows.size() < size_t(level)) rows.resize(size_t(level));
	rows[size_t(level - 1)] = counts;
}

int ClericSpellsTable::Known(int level, int spellLevel) const
{
	if (level < 1 || size_t(level) > rows.size() || spellLevel < 1 || spellLevel > SPELL_LEVELS) return 0;
	return rows[size_t(level - 1)][size_t(spellLevel - 1)];
}

int ArcaneSpellsKnown(int classKnown, int intAdjustment, bool canCast)
{
	if (!canCast || classKnown <= 0) return 0;
	return std::max(1, classKnown + intAdjustment);
}

std::vector<LearnPick> RollArcaneSpells(const SpellLists& lists, int level, int slots, const std::set<std::string>& known,
					const std::function<bool(const std::string&)>& available, bool patronBond, const Roller& roller)
{
	std::vector<LearnPick> picks;
	const std::vector<SpellListEntry>& list = lists.List(CasterKind::Wizard, level);
	if (list.empty() || slots <= 0) return picks;
	std::set<std::string> taken = known; // spells known or picked
	std::set<int> rolledRows; // rows rolled this time (rows without content count too)
	for (int slot = 0; slot < slots; ++slot) {
		LearnPick pick;
		pick.level = level;
		// re-roll duplicates and the patron's spells; give up on rolling after a while (a list
		// whose every row is taken) and go straight to the substitute
		const SpellListEntry* entry = nullptr;
		for (int tries = 0; tries < 100 && !entry; ++tries) {
			int roll = std::min(std::max(roller(int(list.size())), 1), int(list.size()));
			const SpellListEntry& candidate = list[size_t(roll - 1)];
			if ((candidate.flags & LIST_PATRON_SPELL) && !patronBond) continue;
			if (rolledRows.count(candidate.row)) continue;
			if (!candidate.spell.empty() && taken.count(candidate.spell)) continue;
			entry = &candidate;
		}
		if (entry) {
			rolledRows.insert(entry->row);
			pick.row = entry->row;
			pick.rolled = entry->name;
		}
		bool usable = entry && !entry->spell.empty() && !(entry->flags & LIST_CLERIC_SPELL) && available(entry->spell);
		if (usable) {
			pick.spell = entry->spell;
			pick.note = fmt::format("{} level {} row {}: {}", ListName(CasterKind::Wizard), level, entry->row, entry->name);
		} else {
			std::string why;
			if (!entry) {
				why = fmt::format("{} level {}: every row is taken", ListName(CasterKind::Wizard), level);
			} else if (entry->flags & LIST_CLERIC_SPELL) {
				why = fmt::format("{} level {} row {}: {} is a cleric spell, which wizards can't cast yet", ListName(CasterKind::Wizard), level, entry->row, entry->name);
			} else {
				why = fmt::format("{} level {} row {}: {} has no content yet", ListName(CasterKind::Wizard), level, entry->row, entry->name);
			}
			pick.spell = Substitute(list, taken, available, roller);
			pick.substituted = !pick.spell.empty();
			pick.note = pick.substituted ? fmt::format("{}; substituted {}", why, pick.spell) : fmt::format("{}; no spell of that level is left to substitute: the slot stays open", why);
		}
		if (!pick.spell.empty()) taken.insert(pick.spell);
		picks.push_back(std::move(pick));
	}
	return picks;
}

std::vector<LearnPick> PickClericSpells(const SpellLists& lists, int level, int slots, const std::set<std::string>& known,
					const std::function<bool(const std::string&)>& available, const Roller& roller)
{
	std::vector<LearnPick> picks;
	const std::vector<SpellListEntry>& list = lists.List(CasterKind::Cleric, level);
	std::set<std::string> taken = known;
	for (int slot = 0; slot < slots; ++slot) {
		LearnPick pick;
		pick.level = level;
		pick.spell = Substitute(list, taken, available, roller);
		if (pick.spell.empty()) {
			pick.note = fmt::format("{} level {}: every spell with content is known; the slot stays open", ListName(CasterKind::Cleric), level);
		} else {
			for (const auto& entry : list) {
				if (entry.spell == pick.spell) {
					pick.row = entry.row;
					pick.rolled = entry.name;
				}
			}
			pick.note = fmt::format("{} level {} row {}: {} (the god's list: the player may choose another)", ListName(CasterKind::Cleric), level, pick.row, pick.rolled);
			taken.insert(pick.spell);
		}
		picks.push_back(std::move(pick));
	}
	return picks;
}

int ReplaceableSpells(int freshSpells)
{
	return std::max(0, freshSpells / 2);
}

}
}
