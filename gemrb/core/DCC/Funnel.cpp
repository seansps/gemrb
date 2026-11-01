// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Funnel.h"

#include "TableMgr.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace GemRB {
namespace DCC {

bool GodTable::Load(const TableMgr& table)
{
	gods.clear();
	static const char* const columns[] = { "ID", "ALIGN", "NAME" };
	TableMgr::index_t cols[3];
	for (int i = 0; i < 3; ++i) {
		cols[i] = table.GetColumnIndex(std::string(columns[i]));
		if (cols[i] == TableMgr::npos) return false;
	}
	for (TableMgr::index_t r = 0; r < table.GetRowCount(); ++r) {
		God god;
		const std::string& id = table.QueryField(r, cols[0]);
		char* end = nullptr;
		god.id = int(strtol(id.c_str(), &end, 10));
		if (id.empty() || !end || *end || god.id < 1 || god.id > 255) return false;
		std::string align = table.QueryField(r, cols[1]);
		char a = align.empty() ? '?' : char(std::toupper(static_cast<unsigned char>(align[0])));
		if (a == 'L') {
			god.alignment = Alignment::Lawful;
		} else if (a == 'N') {
			god.alignment = Alignment::Neutral;
		} else if (a == 'C') {
			god.alignment = Alignment::Chaotic;
		} else {
			return false;
		}
		god.name = table.QueryField(r, cols[2]);
		std::replace(god.name.begin(), god.name.end(), '_', ' ');
		Set(god);
	}
	return !gods.empty();
}

void GodTable::Set(const God& god)
{
	for (auto& existing : gods) {
		if (existing.id == god.id) {
			existing = god;
			return;
		}
	}
	gods.push_back(god);
}

const God* GodTable::Find(int id) const
{
	if (id <= 0) return nullptr;
	for (const auto& god : gods) {
		if (god.id == id) return &god;
	}
	return nullptr;
}

std::vector<const God*> GodTable::For(Alignment alignment) const
{
	std::vector<const God*> list;
	for (const auto& god : gods) {
		if (god.alignment == alignment) list.push_back(&god);
	}
	return list;
}

int GodTable::Follow(int id, Alignment alignment) const
{
	const God* god = Find(id);
	return god && god->alignment == alignment ? id : 0;
}

int GodTable::Default(Alignment alignment) const
{
	for (const auto& god : gods) {
		if (god.alignment == alignment) return god.id;
	}
	return 0;
}

bool NearDeath(int oldHP, int newHP)
{
	return oldHP >= 2 && newHP >= 1 && newHP < 2;
}

int FunnelPicks(int survivors)
{
	return std::max(0, std::min(survivors, FUNNEL_PARTY_SIZE));
}

unsigned int AlignmentCodeToIE(int code)
{
	// the high nibble is law (1), neutral (2), chaos (3); the low one stays neutral
	return code >= 1 && code <= 3 ? unsigned(code) << 4 | 2 : 0;
}

int AlignmentCode(unsigned int ieAlignment)
{
	switch (AlignmentFromIE(ieAlignment)) {
		case Alignment::Lawful:
			return 1;
		case Alignment::Chaotic:
			return 3;
		default:
			return 2;
	}
}

}
}
