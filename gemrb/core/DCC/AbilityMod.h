// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file AbilityMod.h
 * DCC Table 1-1: one modifier curve shared by all six abilities, plus the two
 * spellcasting columns (wizard spells known adjustment, max spell level).
 *
 * The game data is unhardcoded/ethren/abilmod.2da (rows 3-18, columns MOD,
 * WIZ_SPELL_ADJ and MAX_SPELL_LVL, where a max spell level of 0 means no
 * spellcasting). Interface::GetDCCAbilityBonus reads it; tests can fill a table
 * from plain rows with SetRows instead.
 *
 * Scores outside the table (drain, magic) clamp to its first or last row, so
 * with the stock data anything below 3 counts as 3 and anything above 18 as 18.
 */

#ifndef DCC_ABILITYMOD_H
#define DCC_ABILITYMOD_H

#include "exports.h"

#include <cstdint>
#include <vector>

namespace GemRB {

class TableMgr;

namespace DCC {

struct AbilityModRow {
	int score = 0;
	int mod = 0;
	int wizardSpells = 0; // adjustment to wizard spells known (Int)
	int maxSpellLevel = 0; // highest castable spell level (Int for wizards, Per for clerics), 0: none
};

class GEM_EXPORT AbilityModTable {
public:
	enum class Column : uint8_t { Modifier, WizardSpells, MaxSpellLevel };

	/** Rows may come in any order but must cover consecutive scores once each;
	 * otherwise the table is left empty and this returns false */
	bool SetRows(std::vector<AbilityModRow> newRows);
	/** Reads a 2DA with score row names and MOD, WIZ_SPELL_ADJ and MAX_SPELL_LVL columns */
	bool Load(const TableMgr& table);

	bool IsEmpty() const { return rows.empty(); }
	int MinScore() const;
	int MaxScore() const;

	/** Lookups clamp the score to the table; an empty table gives 0 */
	int Get(Column column, int score) const;
	int Modifier(int score) const { return Get(Column::Modifier, score); }
	int WizardSpells(int score) const { return Get(Column::WizardSpells, score); }
	int MaxSpellLevel(int score) const { return Get(Column::MaxSpellLevel, score); }
	bool CanCast(int score) const { return MaxSpellLevel(score) > 0; }

private:
	std::vector<AbilityModRow> rows; // sorted, rows[i].score == MinScore() + i
};

}
}

#endif
