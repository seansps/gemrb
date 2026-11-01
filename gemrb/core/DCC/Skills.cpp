// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Skills.h"

#include "DCC/Extension.h"

#include "TableMgr.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace GemRB {
namespace DCC {

static std::string Upper(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::toupper(c)); });
	return text;
}

static std::vector<std::string> SplitTags(const std::string& text)
{
	std::vector<std::string> parts;
	std::string part;
	for (char c : text) {
		if (c == '|' || c == ',' || c == ' ' || c == '\t') {
			if (!part.empty()) parts.push_back(part);
			part.clear();
		} else {
			part += c;
		}
	}
	if (!part.empty()) parts.push_back(part);
	return parts;
}

SkillTags ParseSkillTags(const std::string& text)
{
	SkillTags tags;
	std::vector<std::string> parts = SplitTags(text);
	if (parts.empty()) return tags;
	for (const std::string& part : parts) {
		std::string name = Upper(part);
		if (name == "ANYONE") {
			tags.anyone = true;
		} else if (name == "UNTRAINABLE") {
			tags.untrainable = true;
		} else {
			uint32_t bit = OccupationSkillBit(name);
			if (!bit) return SkillTags {};
			tags.bits |= bit;
		}
	}
	tags.valid = true;
	return tags;
}

std::string SkillTagsKey(const SkillTags& tags)
{
	std::string key;
	auto add = [&key](const char* name) {
		if (!key.empty()) key += '|';
		key += name;
	};
	for (int bit = 0; bit < OCCUPATION_SKILL_COUNT; ++bit) {
		if (tags.bits & (1u << bit)) add(OccupationSkillName(bit));
	}
	if (tags.anyone) add("ANYONE");
	if (tags.untrainable) add("UNTRAINABLE");
	return key;
}

int SkillTagCount(uint32_t bits)
{
	int count = 0;
	for (; bits; bits &= bits - 1) ++count;
	return count;
}

const char* SkillTrainingName(SkillTraining training)
{
	static const char* const names[] = { "untrained", "familiar", "trained", "anyone", "thief" };
	size_t i = static_cast<size_t>(training);
	return i < static_cast<size_t>(SkillTraining::count) ? names[i] : "?";
}

static int BitIndex(uint32_t bit)
{
	if (!bit || (bit & (bit - 1))) return -1;
	int index = 0;
	while (!(bit & 1u)) {
		bit >>= 1;
		++index;
	}
	return index;
}

static bool ParseAbility(const std::string& key, Ability& ability)
{
	static const char* const keys[] = { "STR", "AGI", "STA", "PER", "INT", "LUCK" };
	std::string upper = Upper(key);
	for (int i = 0; i < ABILITY_COUNT; ++i) {
		if (upper == keys[i]) {
			ability = static_cast<Ability>(i);
			return true;
		}
	}
	return false;
}

static bool ParseTagList(const std::string& field, uint32_t& bits)
{
	bits = 0;
	if (field == "*" || field.empty()) return true;
	for (const std::string& part : SplitTags(field)) {
		uint32_t bit = OccupationSkillBit(part);
		if (!bit) return false;
		bits |= bit;
	}
	return true;
}

static std::string Spaced(std::string text)
{
	if (text == "*") return std::string();
	std::replace(text.begin(), text.end(), '_', ' ');
	return text;
}

bool SkillTable::Load(const TableMgr& table)
{
	rows = {};
	static const char* const columns[] = { "ABILITY", "FAMILIAR", "THIEF", "CAP", "NAME", "USES" };
	TableMgr::index_t cols[6];
	for (int i = 0; i < 6; ++i) {
		cols[i] = table.GetColumnIndex(std::string(columns[i]));
		if (cols[i] == TableMgr::npos) return false;
	}
	for (TableMgr::index_t r = 0; r < table.GetRowCount(); ++r) {
		SkillInfo info;
		info.bit = OccupationSkillBit(table.GetRowName(r));
		if (!info.bit || !ParseAbility(table.QueryField(r, cols[0]), info.ability)) return false;
		if (!ParseTagList(table.QueryField(r, cols[1]), info.familiar)) return false;
		const std::string& thief = table.QueryField(r, cols[2]);
		if (thief != "*") {
			for (int skill = 0; skill < THIEF_SKILL_COUNT; ++skill) {
				if (Upper(thief) == ThiefSkillKey(static_cast<ThiefSkill>(skill))) info.thief = skill;
			}
			if (info.thief < 0) return false;
		}
		const std::string& cap = table.QueryField(r, cols[3]);
		info.cap = cap == "*" ? 0 : atoi(cap.c_str());
		info.label = Spaced(table.QueryField(r, cols[4]));
		info.uses = Spaced(table.QueryField(r, cols[5]));
		Set(info);
	}
	for (int bit = 0; bit < OCCUPATION_SKILL_COUNT; ++bit) {
		if (!rows[bit].bit) return false;
	}
	return true;
}

void SkillTable::Set(const SkillInfo& info)
{
	int index = BitIndex(info.bit);
	if (index >= 0 && index < OCCUPATION_SKILL_COUNT) rows[index] = info;
}

const SkillInfo* SkillTable::Find(uint32_t bit) const
{
	int index = BitIndex(bit);
	if (index < 0 || index >= OCCUPATION_SKILL_COUNT || !rows[index].bit) return nullptr;
	return &rows[index];
}

uint32_t SkillTable::Familiar(uint32_t bits) const
{
	uint32_t familiar = 0;
	for (int bit = 0; bit < OCCUPATION_SKILL_COUNT; ++bit) {
		if (bits & (1u << bit)) familiar |= rows[bit].familiar;
	}
	return familiar;
}

Ability SkillTable::DefaultAbility(uint32_t bits) const
{
	for (int bit = 0; bit < OCCUPATION_SKILL_COUNT; ++bit) {
		if ((bits & (1u << bit)) && rows[bit].bit) return rows[bit].ability;
	}
	return Ability::Int;
}

std::string SkillTable::Labels(uint32_t bits) const
{
	std::string text;
	for (int bit = 0; bit < OCCUPATION_SKILL_COUNT; ++bit) {
		if (!(bits & (1u << bit))) continue;
		if (!text.empty()) text += ", ";
		const std::string& label = rows[bit].label;
		text += label.empty() ? std::string(OccupationSkillName(bit)) : label;
	}
	return text;
}

bool SkillTable::Empty() const
{
	return std::none_of(rows.begin(), rows.end(), [](const SkillInfo& info) { return info.bit != 0; });
}

bool OccupationTable::Load(const TableMgr& table)
{
	list.clear();
	TableMgr::index_t race = table.GetColumnIndex(std::string("RACE"));
	TableMgr::index_t skills = table.GetColumnIndex(std::string("SKILLS"));
	TableMgr::index_t name = table.GetColumnIndex(std::string("NAME"));
	if (race == TableMgr::npos || skills == TableMgr::npos || name == TableMgr::npos) return false;
	for (TableMgr::index_t r = 0; r < table.GetRowCount(); ++r) {
		OccupationInfo info;
		info.id = atoi(table.GetRowName(r).c_str());
		if (info.id < 1 || info.id > 100) return false;
		info.race = atoi(table.QueryField(r, race).c_str());
		if (!ParseTagList(table.QueryField(r, skills), info.skills)) return false;
		info.name = Spaced(table.QueryField(r, name));
		Set(info);
	}
	return !list.empty();
}

void OccupationTable::Set(const OccupationInfo& info)
{
	for (auto& existing : list) {
		if (existing.id == info.id) {
			existing = info;
			return;
		}
	}
	list.push_back(info);
}

const OccupationInfo* OccupationTable::Find(int id) const
{
	for (const auto& info : list) {
		if (info.id == id) return &info;
	}
	return nullptr;
}

int SkillThiefOverlap(const SkillTable& table, uint32_t bits)
{
	for (int bit = 0; bit < OCCUPATION_SKILL_COUNT; ++bit) {
		if (!(bits & (1u << bit))) continue;
		const SkillInfo* info = table.Find(1u << bit);
		if (info && info->thief >= 0) return info->thief;
	}
	return -1;
}

SkillPlan PlanSkillCheck(const SkillTable& table, const SkillPlanInput& in)
{
	SkillPlan plan;
	plan.abilityMod = in.abilityMod;
	plan.augur = in.augur;
	plan.situational = in.situational;
	const SkillTags& wanted = in.wanted;
	if (wanted.untrainable) return plan; // d10 for everyone, nothing helps

	int thiefSkill = in.thiefSkill >= 0 ? in.thiefSkill : SkillThiefOverlap(table, wanted.bits);
	if (in.thief && thiefSkill >= 0) {
		plan.training = SkillTraining::Thief;
		plan.die = SKILL_TRAINED_DIE;
		plan.thiefBonus = in.thiefBonus;
		plan.matched = in.known & wanted.bits;
		return plan;
	}
	if (wanted.anyone) {
		plan.training = SkillTraining::Anyone;
		plan.die = SKILL_TRAINED_DIE;
		return plan;
	}

	uint32_t matched = in.known & wanted.bits;
	uint32_t uncapped = 0;
	for (int bit = 0; bit < OCCUPATION_SKILL_COUNT; ++bit) {
		uint32_t mask = 1u << bit;
		if (!(matched & mask)) continue;
		const SkillInfo* info = table.Find(mask);
		if (!info || !info->cap || in.dc <= info->cap) uncapped |= mask;
	}
	if (uncapped) {
		plan.training = SkillTraining::Trained;
		plan.die = SKILL_TRAINED_DIE;
		plan.matched = uncapped;
		return plan;
	}
	uint32_t familiar = in.known & table.Familiar(wanted.bits);
	if (matched || familiar) {
		plan.training = SkillTraining::Familiar;
		plan.familiarity = SKILL_FAMILIAR_BONUS;
		plan.capped = matched != 0;
		plan.matched = matched ? matched : familiar;
	}
	return plan;
}

int SkillSuccesses(const SkillPlan& plan, int dc, int rollMod)
{
	int need = dc - plan.Modifier() - rollMod; // the lowest natural that makes it
	if (need <= 1) return plan.die;
	if (need > plan.die) return 0;
	return plan.die - need + 1;
}

bool BetterSkillPlan(const SkillPlan& a, const SkillPlan& b, int dc)
{
	// chances as fractions successes/die, compared crosswise
	long left = long(SkillSuccesses(a, dc)) * b.die;
	long right = long(SkillSuccesses(b, dc)) * a.die;
	if (left != right) return left > right;
	// the average total, doubled to stay whole: die + 1 + 2 x modifier
	return a.die + 1 + 2 * a.Modifier() > b.die + 1 + 2 * b.Modifier();
}

}
}
