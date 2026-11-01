// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Looks.h"

#include "CharAnimations.h"
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

static std::string Upper(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::toupper(c); });
	return text;
}

bool HeldTable::Load(const TableMgr& table)
{
	rows.clear();
	TableMgr::index_t hands = table.GetColumnIndex("HANDS");
	if (hands == TableMgr::npos) return false;
	for (TableMgr::index_t r = 0; r < table.GetRowCount(); ++r) {
		const std::string& value = table.QueryField(r, hands);
		if (value != "1" && value != "2") return false;
		const std::string& key = table.GetRowName(r);
		if (key.empty() || key.size() > 8) return false;
		Set(key, value == "2");
	}
	return true;
}

void HeldTable::Set(const std::string& key, bool bothHands)
{
	rows[Upper(key)] = bothHands;
}

bool HeldTable::BothHands(const std::string& resref, const std::string& animCode) const
{
	if (!resref.empty()) {
		auto row = rows.find(Upper(resref));
		if (row != rows.end()) return row->second;
	}
	std::string code = Code(animCode);
	if (code.size() != 2) return false;
	auto row = rows.find(code);
	return row != rows.end() && row->second;
}

unsigned char WeaponAnimType(bool twoHandedFlag, bool heldInBothHands, OffHand offHand)
{
	if (twoHandedFlag) return IE_ANI_WEAPON_2H;
	if (offHand == OffHand::Weapon) return IE_ANI_WEAPON_2W;
	if (offHand == OffHand::Empty && heldInBothHands) return IE_ANI_WEAPON_2H;
	return IE_ANI_WEAPON_1H;
}

}
}
