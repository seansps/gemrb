// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Rules.h"

#include "GameData.h"
#include "TableMgr.h"

#include "Logging/Logging.h"

namespace GemRB {
namespace DCC {

static const std::array<const char*, IE_SAVE_COUNT> ieSaveRows { "SPELL", "BREATH", "DEATH", "WAND", "POLYMORPH" };

SaveMap::SaveMap()
{
	// the stock mapping, as in unhardcoded/ethren/dccsave.2da
	map = { Save::Will, Save::Ref, Save::Fort, Save::Ref, Save::Fort };
}

bool SaveMap::ParseSave(const std::string& key, Save& save)
{
	for (int i = 0; i < SAVE_COUNT; ++i) {
		if (key == SaveKey(static_cast<Save>(i))) {
			save = static_cast<Save>(i);
			return true;
		}
	}
	return false;
}

const char* SaveMap::SaveKey(Save save)
{
	switch (save) {
		case Save::Fort:
			return "FORT";
		case Save::Ref:
			return "REF";
		case Save::Will:
			return "WILL";
		default:
			return "";
	}
}

bool SaveMap::Load(const TableMgr& table)
{
	auto saveCol = table.GetColumnIndex("SAVE");
	if (saveCol == TableMgr::npos) return false;

	defaultSave = Save::Will;
	auto defaultRow = table.GetRowIndex("DEFAULT");
	if (defaultRow != TableMgr::npos && !ParseSave(table.QueryField(defaultRow, saveCol), defaultSave)) {
		return false;
	}
	for (size_t i = 0; i < ieSaveRows.size(); ++i) {
		auto row = table.GetRowIndex(StringView(ieSaveRows[i]));
		map[i] = defaultSave;
		if (row != TableMgr::npos && !ParseSave(table.QueryField(row, saveCol), map[i])) {
			return false;
		}
	}
	return true;
}

Save SaveMap::Get(IESave type) const
{
	return Get(static_cast<unsigned int>(type));
}

Save SaveMap::Get(unsigned int ieType) const
{
	return ieType < map.size() ? map[ieType] : defaultSave;
}

bool RulesData::Load()
{
	bool ok = true;
	auto fail = [&ok](const char* what) {
		Log(ERROR, "DCC", "Unable to read the '{}' table!", what);
		ok = false;
	};

	AutoTable table = gamedata->LoadTable("dccxp");
	if (!table || !xp.Load(*table)) fail("dccxp");

	table = gamedata->LoadTable("dccclass");
	if (!table || !classes.Load(*table, [](const std::string& name) { return gamedata->LoadTable(ResRef(name)); })) {
		fail("dccclass (or a class progression table)");
	}

	table = gamedata->LoadTable("dccaugur");
	if (!table || !augurs.Load(*table)) fail("dccaugur");

	table = gamedata->LoadTable("dccsave");
	if (!table || !saves.Load(*table)) fail("dccsave");

	static const std::array<std::pair<Alignment, const char*>, 3> thiefTables { { { Alignment::Lawful, "dccthl" }, { Alignment::Chaotic, "dccthc" }, { Alignment::Neutral, "dccthn" } } };
	for (const auto& entry : thiefTables) {
		table = gamedata->LoadTable(ResRef(entry.second));
		if (!table || !thiefSkills.Load(entry.first, *table)) fail(entry.second);
	}
	return ok;
}

}
}
