// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Deeds.h"

#include "TableMgr.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace GemRB {
namespace DCC {

static const std::array<const char*, DEED_COUNT> deedKeys { "", "BLIND", "DISARM", "PUSH", "TRIP", "PRECISION", "RALLY", "DEFEND" };

const char* DeedKey(Deed deed)
{
	size_t i = static_cast<size_t>(deed);
	return i < deedKeys.size() ? deedKeys[i] : "";
}

Deed ParseDeedKey(const std::string& key)
{
	std::string upper = key;
	std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
	for (size_t i = 1; i < deedKeys.size(); ++i) {
		if (upper == deedKeys[i]) return static_cast<Deed>(i);
	}
	return Deed::None;
}

bool DeedSucceeds(int deedRoll, bool hit)
{
	return hit && deedRoll >= DEED_SUCCESS;
}

int DeedStrength(int deedRoll)
{
	if (deedRoll < DEED_SUCCESS) return 0;
	return std::min(deedRoll, DEED_MAX_RESULT);
}

Deed ChooseDeed(Deed queued, Deed signature, Deed weaponDeed)
{
	if (queued != Deed::None) return queued;
	if (signature != Deed::None) return signature;
	return weaponDeed;
}

Deed WeaponDeed(int itemType)
{
	switch (itemType) {
		case 23: // flail
			return Deed::Trip;
		case 21: // hammer
			return Deed::Push;
		default:
			return Deed::None;
	}
}

bool DeedTable::SetRows(Deed deed, std::vector<ResultRow> rows)
{
	size_t i = static_cast<size_t>(deed);
	if (deed == Deed::None || i >= tables.size()) return false;
	if (!tables[i].SetRows(std::move(rows))) return false;
	if (tables[i].MinRoll() != DEED_SUCCESS || tables[i].MaxRoll() != DEED_MAX_RESULT) {
		tables[i] = ResultTable();
		return false;
	}
	return true;
}

bool DeedTable::Load(const TableMgr& table)
{
	tables = {};
	auto diceCol = table.GetColumnIndex("DICE");
	auto effectsCol = table.GetColumnIndex("EFFECTS");
	auto saveCol = table.GetColumnIndex("SAVE");
	auto textCol = table.GetColumnIndex("TEXT");
	if (diceCol == TableMgr::npos || effectsCol == TableMgr::npos || saveCol == TableMgr::npos || textCol == TableMgr::npos) {
		return false;
	}

	std::array<std::vector<ResultRow>, DEED_COUNT> rows;
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		std::string name = table.GetRowName(row);
		// KEY followed by the deed die result
		size_t digit = name.find_first_of("0123456789");
		if (digit == std::string::npos || digit == 0) return false;
		Deed deed = ParseDeedKey(name.substr(0, digit));
		if (deed == Deed::None) return false;
		ResultRow entry;
		entry.roll = std::atoi(name.c_str() + digit);
		if (!ParseDice(table.QueryField(row, diceCol), entry.dice)) return false;
		if (!ParseEffects(table.QueryField(row, effectsCol), entry.effects)) return false;
		if (!ParseSaveSpec(table.QueryField(row, saveCol), entry.save)) return false;
		entry.text = table.QueryField(row, textCol);
		if (entry.text == "*") entry.text.clear();
		std::replace(entry.text.begin(), entry.text.end(), '_', ' ');
		rows[static_cast<size_t>(deed)].push_back(std::move(entry));
	}
	for (int i = 1; i < DEED_COUNT; ++i) {
		if (!SetRows(static_cast<Deed>(i), std::move(rows[i]))) return false;
	}
	return true;
}

bool DeedTable::IsEmpty() const
{
	return std::all_of(tables.begin() + 1, tables.end(), [](const ResultTable& table) { return table.IsEmpty(); });
}

const ResultRow* DeedTable::Lookup(Deed deed, int deedRoll) const
{
	size_t i = static_cast<size_t>(deed);
	if (deed == Deed::None || i >= tables.size() || deedRoll < DEED_SUCCESS) return nullptr;
	return tables[i].Lookup(deedRoll);
}

}
}
