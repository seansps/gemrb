// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Body.h"

#include "TableMgr.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace GemRB {
namespace DCC {

static bool ParseInt(const std::string& text, int& out)
{
	if (text.empty()) return false;
	char* end = nullptr;
	long value = strtol(text.c_str(), &end, 10);
	if (!end || *end) return false;
	out = int(value);
	return true;
}

const char* LoadName(Load load)
{
	switch (load) {
		case Load::Light:
			return "light";
		case Load::Heavy:
			return "heavy";
		case Load::Over:
			return "over";
		default:
			return "?";
	}
}

bool BodyTable::Load(const TableMgr& table)
{
	rows.clear();
	static const char* const columns[] = { "RACE", "BASE", "DICE", "SIDES", "HALF", "CAP" };
	TableMgr::index_t cols[6];
	for (int i = 0; i < 6; ++i) {
		cols[i] = table.GetColumnIndex(std::string(columns[i]));
		if (cols[i] == TableMgr::npos) return false;
	}
	for (TableMgr::index_t r = 0; r < table.GetRowCount(); ++r) {
		int values[6] {};
		for (int i = 0; i < 6; ++i) {
			if (!ParseInt(table.QueryField(r, cols[i]), values[i])) return false;
		}
		BodyRow row { values[0], values[1], values[2], values[3], values[4], values[5] };
		if (row.base < 1 || row.dice < 0 || row.sides < 0 || row.halfPercent < 1 || row.capPercent < row.halfPercent) return false;
		rows.push_back(row);
	}
	return !rows.empty();
}

void BodyTable::Set(const BodyRow& row)
{
	for (auto& existing : rows) {
		if (existing.race == row.race) {
			existing = row;
			return;
		}
	}
	rows.push_back(row);
}

const BodyRow& BodyTable::Row(int race) const
{
	static const BodyRow fallback { 1, 110, 5, 10, 33, 50 };
	if (rows.empty()) return fallback;
	for (const auto& row : rows) {
		if (row.race == race) return row;
	}
	return rows.front();
}

int BodyTable::Average(int race) const
{
	const BodyRow& row = Row(race);
	return row.base + row.dice * (row.sides + 1) / 2;
}

int BodyTable::Roll(int race, const std::function<int(int)>& die) const
{
	const BodyRow& row = Row(race);
	int total = row.base;
	for (int i = 0; i < row.dice; ++i) {
		total += row.sides > 0 ? Clamp(die(row.sides), 1, row.sides) : 0;
	}
	return total;
}

LoadLimits LimitsFor(int bodyWeight, const BodyRow& row)
{
	LoadLimits limits;
	bodyWeight = std::max(bodyWeight, 1);
	limits.half = bodyWeight * row.halfPercent / 100;
	limits.cap = bodyWeight * row.capPercent / 100;
	return limits;
}

Load LoadFor(int carried, const LoadLimits& limits)
{
	if (carried <= limits.half) return Load::Light;
	if (carried <= limits.cap) return Load::Heavy;
	return Load::Over;
}

int SpeedDivisor(Load load)
{
	switch (load) {
		case Load::Light:
			return 1;
		case Load::Heavy:
			return 2;
		default:
			return 0;
	}
}

}
}
