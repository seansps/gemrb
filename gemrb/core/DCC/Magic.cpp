// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Magic.h"

#include "TableMgr.h"
#include "globals.h"

#include "DCC/Classes.h"
#include "DCC/CombatTables.h"
#include "DCC/DiceChain.h"

#include <algorithm>
#include <cctype>

namespace GemRB {
namespace DCC {

static std::string Upper(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::toupper(c); });
	return text;
}

static std::string Lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
	return text;
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

// "*" is an empty cell in our 2DAs
static std::string Cell(const TableMgr& table, TableMgr::index_t row, TableMgr::index_t col)
{
	if (col == TableMgr::npos) return {};
	std::string value = table.QueryField(row, col);
	return value == "*" ? std::string() : value;
}

static const std::array<std::pair<const char*, uint32_t>, 27> mutationNames { {
	{ "PUSTULES", MUT_PUSTULES },
	{ "SKIN", MUT_SKIN },
	{ "GAIT", MUT_GAIT },
	{ "EYES", MUT_EYES },
	{ "EARS", MUT_EARS },
	{ "SHAKES", MUT_SHAKES },
	{ "HAIR", MUT_HAIR },
	{ "WITHER", MUT_WITHER },
	{ "FACE", MUT_FACE },
	{ "BLOAT", MUT_BLOAT },
	{ "CRACKLE", MUT_CRACKLE },
	{ "HEIGHT", MUT_HEIGHT },
	{ "BEAST", MUT_BEAST },
	{ "HORNS", MUT_HORNS },
	{ "TONGUE", MUT_TONGUE },
	{ "TENTACLES", MUT_TENTACLES },
	{ "CLAW", MUT_CLAW },
	{ "TAIL", MUT_TAIL },
	{ "SCALES", MUT_SCALES },
	{ "BEAK", MUT_BEAK },
	{ "MARK", MUT_MARK },
	{ "HANDS", MUT_HANDS },
	{ "GHOST", MUT_GHOST },
	{ "BURNT", MUT_BURNT },
	{ "SLEEPLESS", MUT_SLEEPLESS },
	{ "STENCH", MUT_STENCH },
	{ "AURA", MUT_AURA },
} };

uint32_t MutationBit(const std::string& name)
{
	std::string upper = Upper(name);
	for (const auto& entry : mutationNames) {
		if (upper == entry.first) return entry.second;
	}
	return MUT_NONE;
}

const char* MutationName(uint32_t bit)
{
	static const std::array<const char*, 27> names { "pustules", "melted skin", "an odd gait", "strange eyes", "changed ears", "the shakes",
							   "strange hair", "withered", "a second face", "bloated", "crackling energy", "changed height",
							   "beastly parts", "horns", "a forked tongue", "tentacles", "a crab claw", "a tail", "scales",
							   "a beak", "the mark of the unfaithful", "coloured hands", "ghostly", "burnt", "sleepless",
							   "a stench", "an orbiting stone" };
	for (size_t i = 0; i < mutationNames.size(); ++i) {
		if (mutationNames[i].second == bit) return names[i];
	}
	return "";
}

uint32_t MutationTint(uint32_t bits)
{
	// the most striking first: scales and tentacles green, burns and pustules dark red, ghosts pale
	// blue, beasts brown, the mark of the unfaithful grey; the rest a sickly yellow
	static const std::array<std::pair<uint32_t, uint32_t>, 9> tints { {
		{ MUT_SCALES | MUT_TENTACLES | MUT_CLAW, 0x60c060 },
		{ MUT_GHOST, 0xa0c0ff },
		{ MUT_BURNT | MUT_PUSTULES, 0xc06050 },
		{ MUT_BEAST | MUT_HORNS | MUT_TAIL | MUT_BEAK, 0xb08860 },
		{ MUT_CRACKLE | MUT_AURA, 0xc0c0ff },
		{ MUT_WITHER | MUT_BLOAT, 0xc0b080 },
		{ MUT_MARK, 0x909090 },
		{ MUT_HANDS | MUT_SKIN | MUT_EYES | MUT_HAIR, 0xd0d090 },
		{ ~0u, 0xd8d0a0 },
	} };
	if (!bits) return 0;
	for (const auto& tint : tints) {
		if (bits & tint.first) return tint.second;
	}
	return 0;
}

CasterKind CasterFor(int classID, int level)
{
	if (level < 1) return CasterKind::None;
	switch (static_cast<ClassID>(classID)) {
		case ClassID::Wizard:
		case ClassID::Elf:
			return CasterKind::Wizard;
		case ClassID::Cleric:
			return CasterKind::Cleric;
		default:
			return CasterKind::None;
	}
}

SpellCheckResult ResolveSpellCheck(const SpellCheckInput& input)
{
	SpellCheckResult result;
	result.die = std::max(input.die, 2);
	result.natural = Clamp(input.natural, 1, result.die);
	if (input.autoCrit) result.natural = std::max(result.natural, 20);
	result.total = result.natural + input.casterLevel + input.abilityMod + input.spellburn + input.luck + input.other;
	if (!input.cleric) result.total += input.armorPenalty;
	result.natOne = result.natural == 1 && !input.autoCrit;
	if (input.cleric && InDisapprovalRange(result.natural, input.disapprovalRange) && !input.autoCrit) {
		result.disapproval = true;
	}
	result.critical = result.natural >= 20 && !result.disapproval;
	if (result.critical) result.total += input.casterLevel;
	result.cursed = input.cursed;
	return result;
}

bool ParseNatOne(const std::string& text, uint32_t& outcome)
{
	outcome = NAT1_NONE;
	std::string rest = Upper(text);
	if (rest.empty() || rest == "*" || rest == "NONE") return true;
	size_t start = 0;
	while (start <= rest.size()) {
		size_t end = rest.find('+', start);
		std::string word = rest.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (word == "CORRUPT" || word == "CORRUPTION") {
			outcome |= NAT1_CORRUPT;
		} else if (word == "TAINT") {
			outcome |= NAT1_TAINT;
		} else if (word == "MISFIRE") {
			outcome |= NAT1_MISFIRE;
		} else {
			return false;
		}
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return true;
}

int SpellInfo::BandFor(const SpellCheckResult& check) const
{
	int lowest = -1;
	for (size_t i = 0; i < bands.size(); ++i) {
		if (check.natOne && bands[i].natOne) return int(i);
		if (!bands[i].natOne && lowest < 0) lowest = int(i);
	}
	// a natural 1 without its own row, a cursed or a disapproved check: the lowest failure
	// (Ethren decision: never better than a plain failure)
	if (check.natOne || check.cursed || check.disapproval) return lowest;
	int found = lowest;
	for (size_t i = 0; i < bands.size(); ++i) {
		if (!bands[i].natOne && check.total >= bands[i].min) found = int(i);
	}
	return found;
}

int SpellInfo::FirstSuccess() const
{
	for (const auto& band : bands) {
		if (band.success) return band.min;
	}
	return 0;
}

bool SpellInfo::Load(const std::string& baseRef, const TableMgr& table)
{
	base = Lower(baseRef);
	bands.clear();
	natOne.clear();
	corruption.clear();
	misfire.clear();
	manifestation.clear();
	auto minCol = table.GetColumnIndex("MIN");
	auto lostCol = table.GetColumnIndex("LOST");
	auto okCol = table.GetColumnIndex("OK");
	auto res2Col = table.GetColumnIndex("RES2");
	auto fx2Col = table.GetColumnIndex("EFFECTS2");
	auto text2Col = table.GetColumnIndex("TEXT2");
	auto ok2Col = table.GetColumnIndex("OK2");
	auto resCol = table.GetColumnIndex("RES");
	auto fxCol = table.GetColumnIndex("EFFECTS");
	auto textCol = table.GetColumnIndex("TEXT");
	if (minCol == TableMgr::npos || resCol == TableMgr::npos) return false;
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		std::string name = Upper(table.GetRowName(row));
		if (name.empty()) continue;
		int min = 0;
		if (!ParseInt(Cell(table, row, minCol), min)) return false;
		std::string res = Cell(table, row, resCol);
		std::string effects = Cell(table, row, fxCol);
		std::string text = Cell(table, row, textCol);
		std::replace(text.begin(), text.end(), '_', ' ');
		if (name == "INFO") {
			// the spell's own facts: MIN its level, RES its class, TEXT its name
			level = min;
			kind = Upper(res) == "CLERIC" ? CasterKind::Cleric : CasterKind::Wizard;
			this->name = text;
			continue;
		}
		char rowKind = name[0];
		if (rowKind == 'B') {
			SpellBand band;
			band.min = min;
			band.natOne = name == "BN";
			band.lost = Cell(table, row, lostCol) == "1";
			band.success = Cell(table, row, okCol) == "1";
			band.spell = Lower(res);
			band.effects = effects;
			band.text = text;
			band.spell2 = Lower(Cell(table, row, res2Col));
			band.effects2 = Cell(table, row, fx2Col);
			band.text2 = Cell(table, row, text2Col);
			std::replace(band.text2.begin(), band.text2.end(), '_', ' ');
			band.success2 = Cell(table, row, ok2Col) == "1";
			bands.push_back(std::move(band));
		} else if (rowKind == 'N') {
			NatOneRow nat;
			nat.min = min;
			if (!ParseNatOne(res, nat.outcome)) return false;
			natOne.push_back(nat);
		} else if (rowKind == 'C' || rowKind == 'F' || rowKind == 'M') {
			SpellEntry entry;
			entry.roll = min;
			entry.effects = effects;
			entry.text = text;
			auto& list = rowKind == 'C' ? corruption : (rowKind == 'F' ? misfire : manifestation);
			list.push_back(std::move(entry));
		}
	}
	auto byMin = [](const auto& a, const auto& b) { return a.min < b.min; };
	std::stable_sort(bands.begin(), bands.end(), byMin);
	std::stable_sort(natOne.begin(), natOne.end(), byMin);
	auto byRoll = [](const SpellEntry& a, const SpellEntry& b) { return a.roll < b.roll; };
	std::stable_sort(corruption.begin(), corruption.end(), byRoll);
	std::stable_sort(misfire.begin(), misfire.end(), byRoll);
	std::stable_sort(manifestation.begin(), manifestation.end(), byRoll);
	return !bands.empty();
}

uint32_t NatOneFor(const std::vector<NatOneRow>& rows, int subroll)
{
	uint32_t outcome = rows.empty() ? uint32_t(NAT1_CORRUPT) : rows.front().outcome;
	for (const auto& row : rows) {
		if (subroll >= row.min) outcome = row.outcome;
	}
	return outcome;
}

std::vector<int> StandardBands(CasterKind kind, int level)
{
	level = Clamp(level, 1, 5);
	if (kind == CasterKind::Cleric) {
		static const std::array<std::vector<int>, 5> cleric = { {
			{ 1, 12, 14, 18, 20, 24, 28, 30, 32 },
			{ 1, 14, 16, 20, 22, 26, 30, 32, 34 },
			{ 1, 16, 18, 22, 24, 27, 32, 34, 36 },
			{ 1, 18, 20, 24, 26, 28, 34, 36, 38 },
			{ 1, 20, 22, 26, 28, 30, 36, 38 },
		} };
		return cleric[level - 1];
	}
	static const std::array<std::vector<int>, 5> wizard = { {
		{ 1, 2, 12, 14, 18, 20, 24, 28, 30, 32 },
		{ 1, 2, 12, 14, 16, 20, 22, 26, 30, 32, 34 },
		{ 1, 2, 12, 16, 18, 22, 24, 27, 32, 34, 36 },
		{ 1, 2, 12, 18, 20, 24, 26, 29, 34, 36, 38 },
		{ 1, 2, 12, 18, 20, 24, 26, 29, 34, 36, 38 },
	} };
	return wizard[level - 1];
}

const SpellEntry* EntryFor(const std::vector<SpellEntry>& list, int roll)
{
	if (list.empty()) return nullptr;
	const SpellEntry* found = &list.front();
	for (const auto& entry : list) {
		if (entry.roll <= roll) found = &entry;
	}
	return found;
}

int CorruptionRoll(int d10, int spellLevel, int luckMod)
{
	return Clamp(d10 - spellLevel + luckMod, 1, 10);
}

int MercurialRoll(int d100, int luckMod)
{
	return Clamp(d100 + luckMod * 10, 1, 100);
}

int MercurialReroll(int fourD20, int luckMod)
{
	return Clamp(fourD20 + luckMod * 10, 1, 100);
}

int MercurialDieSteps(int entry)
{
	switch (entry) {
		case MERC_DIE_DOWN2:
			return -2;
		case MERC_DIE_DOWN1:
			return -1;
		case MERC_DIE_UP1:
			return 1;
		case MERC_DIE_UP2:
			return 2;
		default:
			return 0;
	}
}

int MercurialDie(int actionDie, const std::vector<uint8_t>& entries, int d6ForEntry38)
{
	int steps = 0;
	bool random = false;
	bool twoD10 = false;
	for (uint8_t entry : entries) {
		steps += MercurialDieSteps(entry);
		if (entry == MERC_RANDOM_DIE) random = true;
		if (entry == MERC_TWO_D10) twoD10 = true;
	}
	if (random) {
		if (d6ForEntry38 <= 2) return 16;
		if (d6ForEntry38 <= 4) return 20;
		return d6ForEntry38 == 5 ? 24 : 30;
	}
	(void) twoD10; // 2d10 keeps the die's size; the glue rolls two halves
	return DiceChain::Shift(actionDie, steps);
}

int SpellburnMax(const std::array<int, 3>& current)
{
	int total = 0;
	for (int score : current) {
		total += std::max(0, score - 1);
	}
	return total;
}

bool SpellburnLegal(const std::array<int, 3>& points, const std::array<int, 3>& current)
{
	for (size_t i = 0; i < points.size(); ++i) {
		if (points[i] < 0) return false;
		if (points[i] > std::max(0, current[i] - 1)) return false;
	}
	return true;
}

int SpellburnBonus(int points, bool recastsLost, int spellLevel)
{
	if (points <= 0) return recastsLost ? -1 : 0;
	if (!recastsLost) return points;
	if (points < spellLevel) return -1;
	return points - spellLevel;
}

int SpellburnRecoveryIndex(const std::array<uint8_t, 3>& points)
{
	int best = -1;
	for (int i = 0; i < 3; ++i) {
		if (points[i] == 0) continue;
		if (best < 0 || points[i] > points[best]) best = i;
	}
	return best;
}

bool InDisapprovalRange(int natural, int range)
{
	return natural <= std::max(1, range);
}

int DisapprovalRoll(int d4Sum, int luckMod)
{
	return Clamp(d4Sum - luckMod, 1, DISAPPROVAL_MAX);
}

int DisapprovalWidened(int range)
{
	return std::min(std::max(1, range) + 1, DISAPPROVAL_MAX);
}

int DisapprovalAfterSacrifice(int range, int gold)
{
	if (gold <= 0) return std::max(1, range);
	return std::max(1, range - gold / ATONEMENT_GOLD);
}

Kinship KinshipFor(Alignment cleric, Alignment target)
{
	if (cleric == target) return Kinship::Same;
	if (cleric == Alignment::Neutral || target == Alignment::Neutral) return Kinship::Adjacent;
	return Kinship::Opposed;
}

int LayOnHandsDice(int total, Kinship kinship, int targetHD)
{
	// classes.md "Lay on hands": 12-13, 14-19, 20-21, 22+ by same / adjacent / opposed
	static const std::array<std::array<int, 3>, 4> dice = { {
		{ 2, 1, 1 },
		{ 3, 2, 1 },
		{ 4, 3, 2 },
		{ 5, 4, 3 },
	} };
	int row = -1;
	if (total >= 22) {
		row = 3;
	} else if (total >= 20) {
		row = 2;
	} else if (total >= 14) {
		row = 1;
	} else if (total >= 12) {
		row = 0;
	}
	if (row < 0) return 0;
	int count = dice[size_t(row)][static_cast<size_t>(kinship)];
	return std::min(count, std::max(1, targetHD));
}

int TurnCell::Count(int casterLevel, const Roller& roller) const
{
	int count = bonus + (addCL ? casterLevel : 0);
	for (int i = 0; i < dice; ++i) {
		count += roller(std::max(1, sides));
	}
	return kind == Kind::None ? 0 : std::max(count, 1);
}

static bool ParseCountDice(const std::string& text, int& dice, int& sides, int& bonus, bool& addCL)
{
	dice = 0;
	sides = 0;
	bonus = 0;
	addCL = false;
	std::string rest = text;
	auto plus = rest.find("+CL");
	if (plus != std::string::npos) {
		addCL = true;
		rest = rest.substr(0, plus);
	}
	auto d = rest.find('D');
	if (d == std::string::npos) return ParseInt(rest, bonus);
	if (!ParseInt(rest.substr(0, d), dice)) return false;
	return ParseInt(rest.substr(d + 1), sides) && sides > 0;
}

bool ParseTurnCell(const std::string& text, TurnCell& cell)
{
	cell = TurnCell();
	std::string upper = Upper(text);
	if (upper.empty() || upper == "NE" || upper == "*") return true;
	size_t start = 0;
	bool first = true;
	while (start <= upper.size()) {
		size_t end = upper.find(';', start);
		std::string part = upper.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (first) {
			if (part.empty() || (part[0] != 'T' && part[0] != 'K')) return false;
			cell.kind = part[0] == 'T' ? TurnCell::Kind::Turn : TurnCell::Kind::Kill;
			if (!ParseCountDice(part.substr(1), cell.dice, cell.sides, cell.bonus, cell.addCL)) return false;
			first = false;
		} else if (part == "NS") {
			cell.noSave = true;
		} else if (!part.empty() && part[0] == 'D') {
			int bonus = 0;
			bool cl = false;
			if (!ParseCountDice(part.substr(1), cell.damageDice, cell.damageSides, bonus, cl)) return false;
		} else {
			return false;
		}
		if (end == std::string::npos) break;
		start = end + 1;
	}
	if (cell.kind == TurnCell::Kind::Kill) cell.noSave = true;
	return true;
}

int TurnColumn(int hd)
{
	if (hd <= 1) return 0;
	if (hd == 2) return 1;
	if (hd <= 4) return 2;
	if (hd <= 6) return 3;
	if (hd <= 8) return 4;
	if (hd <= 10) return 5;
	return 6;
}

bool MagicTableList::Load(const TableMgr& table)
{
	rows.clear();
	auto fxCol = table.GetColumnIndex("EFFECTS");
	auto textCol = table.GetColumnIndex("TEXT");
	auto subCol = table.GetColumnIndex("SUB");
	if (fxCol == TableMgr::npos || textCol == TableMgr::npos) return false;
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		std::string name = table.GetRowName(row);
		// a single roll ("5") or a range ("41-60"): the range's first roll keys it
		auto dash = name.find('-', 1);
		MagicEntry entry;
		if (!ParseInt(dash == std::string::npos ? name : name.substr(0, dash), entry.roll)) return false;
		entry.effects = Cell(table, row, fxCol);
		entry.text = Cell(table, row, textCol);
		std::replace(entry.text.begin(), entry.text.end(), '_', ' ');
		std::string sub = Cell(table, row, subCol);
		if (!sub.empty()) {
			if (sub[0] == 'd' || sub[0] == 'D') sub = sub.substr(1);
			if (!ParseInt(sub, entry.subDie)) return false;
		}
		rows.push_back(std::move(entry));
	}
	std::stable_sort(rows.begin(), rows.end(), [](const MagicEntry& a, const MagicEntry& b) { return a.roll < b.roll; });
	return !rows.empty();
}

const MagicEntry* MagicTableList::Lookup(int roll) const
{
	if (rows.empty()) return nullptr;
	const MagicEntry* found = &rows.front();
	for (const auto& row : rows) {
		if (row.roll <= roll) found = &row;
	}
	return found;
}

bool MagicTables::LoadTurn(const TableMgr& table)
{
	turn.clear();
	auto rangeCol = table.GetColumnIndex("RANGE");
	auto smiteCol = table.GetColumnIndex("SMITE");
	if (rangeCol == TableMgr::npos || smiteCol == TableMgr::npos) return false;
	static const std::string columns[] = { "HD1", "HD2", "HD3", "HD5", "HD7", "HD9", "HD11" };
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		TurnRow entry;
		if (!ParseInt(table.GetRowName(row), entry.min)) return false;
		std::string range = Cell(table, row, rangeCol);
		if (!range.empty() && !ParseInt(range, entry.range)) return false;
		std::string smite = Upper(Cell(table, row, smiteCol));
		if (!smite.empty()) {
			auto d = smite.find('D');
			if (d == std::string::npos || !ParseInt(smite.substr(0, d), entry.smiteDice) || !ParseInt(smite.substr(d + 1), entry.smiteSides)) return false;
		}
		for (size_t c = 0; c < 7; ++c) {
			auto col = table.GetColumnIndex(columns[c]);
			if (col == TableMgr::npos || !ParseTurnCell(Cell(table, row, col), entry.cells[c])) return false;
		}
		entry.cells[7] = entry.cells[6];
		turn.push_back(entry);
	}
	std::stable_sort(turn.begin(), turn.end(), [](const TurnRow& a, const TurnRow& b) { return a.min < b.min; });
	return !turn.empty();
}

const TurnRow* MagicTables::TurnFor(int total) const
{
	const TurnRow* found = nullptr;
	for (const auto& row : turn) {
		if (total >= row.min || !found) found = &row;
	}
	return found;
}

}
}
