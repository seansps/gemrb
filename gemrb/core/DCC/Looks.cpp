// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Looks.h"

#include "TableMgr.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace GemRB {
namespace DCC {

// the overlay part's palette types (CharAnimations.h's PaletteType)
constexpr unsigned int PALETTE_WEAPON = 5;
constexpr unsigned int PALETTE_HELMET = 7;

static std::string Code(const std::string& text)
{
	if (text.empty() || text[0] == '*') return {};
	std::string code = text.substr(0, 2);
	std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return std::toupper(c); });
	return code;
}

bool HairTable::Load(const TableMgr& table)
{
	styles.clear();
	static const char* const columns[] = { "ID", "KIND", "CODE", "HELMET" };
	TableMgr::index_t cols[4];
	for (int i = 0; i < 4; ++i) {
		cols[i] = table.GetColumnIndex(std::string(columns[i]));
		if (cols[i] == TableMgr::npos) return false;
	}
	for (TableMgr::index_t r = 0; r < table.GetRowCount(); ++r) {
		HairStyle style;
		const std::string& id = table.QueryField(r, cols[0]);
		char* end = nullptr;
		style.id = int(strtol(id.c_str(), &end, 10));
		if (id.empty() || !end || *end || style.id < 1 || style.id > 255) return false;
		std::string kind = table.QueryField(r, cols[1]);
		std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char c) { return std::toupper(c); });
		if (kind != "HAIR" && kind != "BEARD") return false;
		style.beard = kind == "BEARD";
		style.code = Code(table.QueryField(r, cols[2]));
		if (style.code.size() != 2) return false;
		style.underHelmet = Code(table.QueryField(r, cols[3]));
		Set(style);
	}
	return true;
}

void HairTable::Set(const HairStyle& style)
{
	for (auto& existing : styles) {
		if (existing.id == style.id && existing.beard == style.beard) {
			existing = style;
			return;
		}
	}
	styles.push_back(style);
}

const HairStyle* HairTable::Find(int id, bool beard) const
{
	if (id <= 0) return nullptr;
	for (const auto& style : styles) {
		if (style.id == id && style.beard == beard) return &style;
	}
	return nullptr;
}

std::string HairOverlay(const HairStyle* style, bool helmet)
{
	if (!style) return {};
	return helmet ? style->underHelmet : style->code;
}

unsigned int OverlayColourByte(unsigned int paletteType)
{
	if (paletteType < PALETTE_WEAPON || paletteType > PALETTE_HELMET) return 0;
	return paletteType - PALETTE_WEAPON + 1;
}

bool CombatStance(bool engaged, bool partyFight)
{
	return engaged || partyFight;
}

}
}
