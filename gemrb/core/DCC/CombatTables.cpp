// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/CombatTables.h"

#include "GameData.h"
#include "TableMgr.h"
#include "globals.h"

#include "Logging/Logging.h"

#include <algorithm>
#include <cctype>

namespace GemRB {
namespace DCC {

static std::string Upper(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::toupper(c); });
	return text;
}

static std::vector<std::string> Split(const std::string& text, char separator)
{
	std::vector<std::string> parts;
	std::string::size_type start = 0;
	while (true) {
		auto end = text.find(separator, start);
		parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return parts;
}

static bool ParseInt(const std::string& text, int& value)
{
	if (text.empty()) return false;
	size_t i = (text[0] == '-' || text[0] == '+') ? 1 : 0;
	if (i == text.size()) return false;
	for (size_t j = i; j < text.size(); ++j) {
		if (!std::isdigit(static_cast<unsigned char>(text[j]))) return false;
	}
	value = std::stoi(text);
	return true;
}

bool ParseDice(const std::string& text, DiceExpr& dice)
{
	dice = DiceExpr();
	if (text.empty() || text == "*" || text == "-") return true;
	std::string lower = text;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

	auto d = lower.find('d');
	if (d == std::string::npos) {
		return ParseInt(lower, dice.add);
	}
	std::string count = lower.substr(0, d);
	std::string rest = lower.substr(d + 1);
	std::string sides = rest;
	std::string add;
	auto sign = rest.find_first_of("+-");
	if (sign != std::string::npos) {
		sides = rest.substr(0, sign);
		add = rest.substr(sign);
	}
	dice.count = 1;
	if (!count.empty() && !ParseInt(count, dice.count)) return false;
	if (!ParseInt(sides, dice.sides) || dice.sides < 1 || dice.count < 0) return false;
	if (!add.empty() && !ParseInt(add, dice.add)) return false;
	return true;
}

int RollDice(const DiceExpr& dice, const Roller& roller)
{
	int total = dice.add;
	for (int i = 0; i < dice.count; ++i) {
		total += roller(dice.sides);
	}
	return total;
}

std::string DiceString(const DiceExpr& dice)
{
	std::string text;
	if (dice.count) {
		text = std::to_string(dice.count) + "d" + std::to_string(dice.sides);
		if (dice.add > 0) text += "+";
	}
	if (dice.add || !dice.count) text += std::to_string(dice.add);
	return text;
}

bool ParseDuration(const std::string& text, Duration& duration)
{
	duration = Duration();
	if (text.empty()) {
		duration.dice.add = 1;
		return true;
	}
	std::string upper = Upper(text);
	if (upper == "PERM") {
		duration.kind = Duration::Kind::Permanent;
		return true;
	}
	if (upper == "HEAL") {
		duration.kind = Duration::Kind::UntilHealed;
		return true;
	}
	std::string amount = upper;
	switch (upper.back()) {
		case 'T':
			duration.kind = Duration::Kind::Turns;
			break;
		case 'H':
			duration.kind = Duration::Kind::Hours;
			break;
		case 'D':
			duration.kind = Duration::Kind::Days;
			break;
		case 'W':
			duration.kind = Duration::Kind::Weeks;
			break;
		case 'R':
			break;
		default:
			amount += 'R';
			break;
	}
	amount.pop_back();
	return ParseDice(amount, duration.dice) && !duration.dice.IsNone();
}

int DurationSeconds(Duration::Kind kind, int amount, int roundSeconds)
{
	switch (kind) {
		case Duration::Kind::Rounds:
			return amount * roundSeconds;
		case Duration::Kind::Turns:
			return amount * 600;
		case Duration::Kind::Hours:
			return amount * 3600;
		case Duration::Kind::Days:
			return amount * 86400;
		case Duration::Kind::Weeks:
			return amount * 604800;
		default:
			return 0;
	}
}

bool ParseSaveSpec(const std::string& text, SaveSpec& save)
{
	save = SaveSpec();
	if (text.empty() || text == "*") return true;
	auto parts = Split(Upper(text), ':');
	if (parts.size() < 2 || parts.size() > 3) return false;
	if (parts[0] == "FORT") {
		save.save = Save::Fort;
	} else if (parts[0] == "REF") {
		save.save = Save::Ref;
	} else if (parts[0] == "WILL") {
		save.save = Save::Will;
	} else {
		return false;
	}
	if (!ParseInt(parts[1], save.dc) || save.dc <= 0) return false;
	if (parts.size() == 3) {
		if (parts[2] != "L") return false;
		save.plusLevel = true;
	}
	return true;
}

bool ParseEffects(const std::string& text, std::vector<TableAlternatives>& effects)
{
	effects.clear();
	if (text.empty() || text == "*") return true;
	for (const auto& termList : Split(text, ',')) {
		TableAlternatives alternatives;
		for (const auto& raw : Split(termList, '|')) {
			if (raw.empty()) return false;
			TableTerm term;
			std::string body = raw;
			if (body[0] == '?' || body[0] == '!') {
				term.gate = body[0];
				body.erase(0, 1);
			}
			auto at = body.find('@');
			if (at != std::string::npos) {
				term.condition = Upper(body.substr(at + 1));
				body.erase(at);
			}
			auto parts = Split(body, ':');
			term.key = Upper(parts[0]);
			if (term.key.empty()) return false;
			term.args.assign(parts.begin() + 1, parts.end());
			alternatives.push_back(std::move(term));
		}
		effects.push_back(std::move(alternatives));
	}
	return true;
}

bool ResultTable::SetRows(std::vector<ResultRow> newRows)
{
	rows.clear();
	if (newRows.empty()) return false;
	std::sort(newRows.begin(), newRows.end(), [](const ResultRow& a, const ResultRow& b) { return a.roll < b.roll; });
	for (size_t i = 1; i < newRows.size(); ++i) {
		if (newRows[i].roll != newRows[i - 1].roll + 1) return false;
	}
	rows = std::move(newRows);
	return true;
}

bool ResultTable::Load(const TableMgr& table)
{
	rows.clear();
	auto diceCol = table.GetColumnIndex("DICE");
	auto effectsCol = table.GetColumnIndex("EFFECTS");
	auto saveCol = table.GetColumnIndex("SAVE");
	auto textCol = table.GetColumnIndex("TEXT");
	if (diceCol == TableMgr::npos || effectsCol == TableMgr::npos || saveCol == TableMgr::npos || textCol == TableMgr::npos) {
		return false;
	}

	std::vector<ResultRow> newRows;
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		ResultRow entry;
		if (!valid_signednumber(table.GetRowName(row).c_str(), entry.roll)) return false;
		if (!ParseDice(table.QueryField(row, diceCol), entry.dice)) return false;
		if (!ParseEffects(table.QueryField(row, effectsCol), entry.effects)) return false;
		if (!ParseSaveSpec(table.QueryField(row, saveCol), entry.save)) return false;
		entry.text = table.QueryField(row, textCol);
		if (entry.text == "*") entry.text.clear();
		std::replace(entry.text.begin(), entry.text.end(), '_', ' ');
		newRows.push_back(std::move(entry));
	}
	return SetRows(std::move(newRows));
}

const ResultRow* ResultTable::Lookup(int roll) const
{
	if (rows.empty()) return nullptr;
	roll = std::min(std::max(roll, MinRoll()), MaxRoll());
	return &rows[roll - MinRoll()];
}

static const std::array<const char*, static_cast<size_t>(CritTableID::count)> critTableNames { "", "I", "II", "III", "IV", "V", "M", "DR", "DN", "G", "U" };

const char* CritTableName(CritTableID id)
{
	size_t i = static_cast<size_t>(id);
	return i < critTableNames.size() ? critTableNames[i] : "";
}

CritTableID ParseCritTableName(const std::string& name)
{
	std::string upper = Upper(name);
	for (size_t i = 1; i < critTableNames.size(); ++i) {
		if (upper == critTableNames[i]) return static_cast<CritTableID>(i);
	}
	return CritTableID::None;
}

static const std::array<const char*, MONSTER_TYPE_COUNT> monsterTypeKeys { "OTHER", "HUMANOID", "DRAGON", "DEMON", "GIANT", "UNDEAD" };

const char* MonsterTypeKey(MonsterType type)
{
	size_t i = static_cast<size_t>(type);
	return i < monsterTypeKeys.size() ? monsterTypeKeys[i] : "";
}

bool ParseMonsterCrit(const std::string& text, MonsterCrit& crit)
{
	crit = MonsterCrit();
	if (text == "-" || text == "*") return true;
	auto slash = text.find('/');
	if (slash == std::string::npos) return false;
	crit.table = ParseCritTableName(text.substr(0, slash));
	DiceExpr dice;
	if (crit.table == CritTableID::None || !ParseDice(text.substr(slash + 1), dice) || dice.count < 1 || dice.add) return false;
	crit.dice = dice.count;
	crit.die = dice.sides;
	return true;
}

bool MonsterCritTable::SetRows(std::vector<Row> newRows)
{
	rows = std::move(newRows);
	return !rows.empty();
}

bool MonsterCritTable::Load(const TableMgr& table)
{
	rows.clear();
	std::array<TableMgr::index_t, MONSTER_TYPE_COUNT> cols;
	for (int i = 0; i < MONSTER_TYPE_COUNT; ++i) {
		cols[i] = table.GetColumnIndex(StringView(monsterTypeKeys[i]));
		if (cols[i] == TableMgr::npos) return false;
	}
	std::vector<Row> newRows;
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		int hd = -1;
		if (!valid_signednumber(table.GetRowName(row).c_str(), hd) || hd != int(row)) return false;
		Row entry;
		for (int i = 0; i < MONSTER_TYPE_COUNT; ++i) {
			if (!ParseMonsterCrit(table.QueryField(row, cols[i]), entry[i])) return false;
		}
		newRows.push_back(entry);
	}
	return SetRows(std::move(newRows));
}

MonsterCrit MonsterCritTable::Lookup(int hd, MonsterType type) const
{
	if (rows.empty()) return {};
	hd = std::min(std::max(hd, 0), int(rows.size()) - 1);
	return rows[hd][static_cast<size_t>(type) < rows[hd].size() ? static_cast<size_t>(type) : 0];
}

bool ArmorFumbleTable::Load(const TableMgr& table)
{
	dice.clear();
	auto dieCol = table.GetColumnIndex("DIE");
	if (dieCol == TableMgr::npos) return false;
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		int die = 0;
		if (!valid_signednumber(table.QueryField(row, dieCol).c_str(), die) || die < 0) return false;
		dice[Upper(table.GetRowName(row))] = die;
	}
	return !dice.empty();
}

int ArmorFumbleTable::Find(const std::string& key) const
{
	auto it = dice.find(Upper(key));
	return it == dice.end() ? -1 : it->second;
}

int ArmorFumbleTable::ArmorDie(const std::string& resref, const std::string& animation) const
{
	int die = resref.empty() ? -1 : Find(resref);
	if (die < 0 && !animation.empty()) die = Find("ANIM_" + animation);
	return std::max(0, die);
}

int ArmorFumbleTable::ShieldDie(const std::string& resref) const
{
	int die = resref.empty() ? -1 : Find(resref);
	if (die < 0) die = Find("SHIELD");
	return std::max(0, die);
}

bool ParseCritRule(const std::string& text, CritRule& rule)
{
	std::string upper = Upper(text);
	if (upper == "NONE") {
		rule = CritRule::None;
	} else if (upper == "MAX") {
		rule = CritRule::MaxOnHit;
	} else if (upper == "MAX_AUTO") {
		rule = CritRule::MaxAutoHit;
	} else if (upper == "NORMAL") {
		rule = CritRule::Normal;
	} else {
		return false;
	}
	return true;
}

bool TwoWeaponTable::SetRows(std::vector<TwoWeaponRow> newRows)
{
	rows.clear();
	for (size_t i = 1; i < newRows.size(); ++i) {
		if (newRows[i].maxAgility <= newRows[i - 1].maxAgility) return false;
	}
	rows = std::move(newRows);
	return !rows.empty();
}

bool TwoWeaponTable::Load(const TableMgr& table)
{
	rows.clear();
	auto primaryCol = table.GetColumnIndex("PRIMARY");
	auto offCol = table.GetColumnIndex("OFF");
	auto critCol = table.GetColumnIndex("CRIT");
	if (primaryCol == TableMgr::npos || offCol == TableMgr::npos || critCol == TableMgr::npos) return false;
	std::vector<TwoWeaponRow> newRows;
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		TwoWeaponRow entry;
		if (!valid_signednumber(table.GetRowName(row).c_str(), entry.maxAgility)) return false;
		if (!valid_signednumber(table.QueryField(row, primaryCol).c_str(), entry.primarySteps)) return false;
		if (!valid_signednumber(table.QueryField(row, offCol).c_str(), entry.offSteps)) return false;
		if (!ParseCritRule(table.QueryField(row, critCol), entry.primaryCrit)) return false;
		newRows.push_back(entry);
	}
	return SetRows(std::move(newRows));
}

TwoWeaponDice TwoWeaponTable::Lookup(int agility, bool halfling) const
{
	TwoWeaponDice dice;
	if (rows.empty()) return dice;
	// halflings count as Agi 16 at least; above 16 they use the table like anyone (classes.md)
	bool halflingRule = halfling && agility <= HALFLING_TWO_WEAPON_AGILITY;
	if (halfling) agility = std::max(agility, HALFLING_TWO_WEAPON_AGILITY);
	const TwoWeaponRow* found = &rows.back();
	for (const auto& row : rows) {
		if (agility <= row.maxAgility) {
			found = &row;
			break;
		}
	}
	dice.primarySteps = found->primarySteps;
	dice.offSteps = found->offSteps;
	dice.primaryCrit = found->primaryCrit;
	dice.offCrit = CritRule::None;
	// and with the halfling rule a natural 16 on either die is an automatic hit and a crit
	if (halflingRule) {
		dice.primaryCrit = CritRule::MaxAutoHit;
		dice.offCrit = CritRule::MaxAutoHit;
	}
	return dice;
}

const ResultTable* CombatTables::Crit(CritTableID id) const
{
	size_t i = static_cast<size_t>(id);
	if (i >= crits.size() || id == CritTableID::None) return nullptr;
	if (crits[i].IsEmpty() && id > CritTableID::M) i = static_cast<size_t>(CritTableID::M);
	return crits[i].IsEmpty() ? nullptr : &crits[i];
}

bool CombatTables::Load()
{
	bool ok = true;
	auto fail = [&ok](const char* what) {
		Log(ERROR, "DCC", "Unable to read the '{}' table!", what);
		ok = false;
	};

	static const std::array<std::pair<CritTableID, const char*>, 6> critFiles { { { CritTableID::I, "dcccrit1" }, { CritTableID::II, "dcccrit2" }, { CritTableID::III, "dcccrit3" }, { CritTableID::IV, "dcccrit4" }, { CritTableID::V, "dcccrit5" }, { CritTableID::M, "dcccritm" } } };
	for (const auto& file : critFiles) {
		AutoTable table = gamedata->LoadTable(ResRef(file.second));
		if (!table || !crits[static_cast<size_t>(file.first)].Load(*table)) fail(file.second);
	}
	AutoTable table = gamedata->LoadTable("dccfumbl");
	if (!table || !fumbles.Load(*table)) fail("dccfumbl");
	table = gamedata->LoadTable("dccmcrit");
	if (!table || !monsterCrits.Load(*table)) fail("dccmcrit");
	table = gamedata->LoadTable("dccfumdi");
	if (!table || !armorFumbles.Load(*table)) fail("dccfumdi");
	table = gamedata->LoadTable("dcctwowp");
	if (!table || !twoWeapons.Load(*table)) fail("dcctwowp");
	return ok;
}

}
}
