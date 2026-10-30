// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/AbilityMod.h"

#include "TableMgr.h"
#include "globals.h"

#include <algorithm>

namespace GemRB {
namespace DCC {

bool AbilityModTable::SetRows(std::vector<AbilityModRow> newRows)
{
	rows.clear();
	if (newRows.empty()) return false;

	std::sort(newRows.begin(), newRows.end(), [](const AbilityModRow& a, const AbilityModRow& b) {
		return a.score < b.score;
	});
	for (size_t i = 1; i < newRows.size(); ++i) {
		if (newRows[i].score != newRows[i - 1].score + 1) return false;
	}

	rows = std::move(newRows);
	return true;
}

bool AbilityModTable::Load(const TableMgr& table)
{
	rows.clear();
	auto modCol = table.GetColumnIndex("MOD");
	auto wizCol = table.GetColumnIndex("WIZ_SPELL_ADJ");
	auto maxCol = table.GetColumnIndex("MAX_SPELL_LVL");
	if (modCol == TableMgr::npos || wizCol == TableMgr::npos || maxCol == TableMgr::npos) {
		return false;
	}

	std::vector<AbilityModRow> newRows;
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		AbilityModRow entry;
		if (!valid_signednumber(table.GetRowName(row).c_str(), entry.score)) return false;
		entry.mod = table.QueryFieldSigned<int>(row, modCol);
		entry.wizardSpells = table.QueryFieldSigned<int>(row, wizCol);
		entry.maxSpellLevel = table.QueryFieldSigned<int>(row, maxCol);
		newRows.push_back(entry);
	}
	return SetRows(std::move(newRows));
}

int AbilityModTable::MinScore() const
{
	return rows.empty() ? 0 : rows.front().score;
}

int AbilityModTable::MaxScore() const
{
	return rows.empty() ? 0 : rows.back().score;
}

int AbilityModTable::Get(Column column, int score) const
{
	if (rows.empty()) return 0;

	const AbilityModRow& row = rows[Clamp(score, MinScore(), MaxScore()) - MinScore()];
	switch (column) {
		case Column::Modifier:
			return row.mod;
		case Column::WizardSpells:
			return row.wizardSpells;
		case Column::MaxSpellLevel:
			return row.maxSpellLevel;
	}
	return 0;
}

}
}
