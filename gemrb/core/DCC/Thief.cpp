// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Thief.h"

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

static std::string Lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return text;
}

static std::string Spaced(std::string text)
{
	if (text == "*") return std::string();
	std::replace(text.begin(), text.end(), '_', ' ');
	return text;
}

static const char* const kindNames[] = { "WOODLAND", "EARTHWORK", "STONEWORK", "WOODWORK", "MECHANISM", "CAMP", "WARD" };

const char* TrapKindName(TrapKind kind)
{
	size_t i = static_cast<size_t>(kind);
	return i < static_cast<size_t>(TrapKind::count) ? kindNames[i] : "?";
}

bool ParseTrapKind(const std::string& name, TrapKind& kind)
{
	std::string upper = Upper(name);
	for (size_t i = 0; i < static_cast<size_t>(TrapKind::count); ++i) {
		if (upper == kindNames[i]) {
			kind = static_cast<TrapKind>(i);
			return true;
		}
	}
	return false;
}

bool DwarfKnowsTrap(TrapKind kind)
{
	return kind == TrapKind::Stonework || kind == TrapKind::Earthwork;
}

uint16_t TrapSlotBit(int inParty)
{
	if (inParty < 1 || inParty > TRAP_SLOTS) return 0;
	return uint16_t(1u << (inParty - 1));
}

static std::string TrapKey(const std::string& area, const std::string& object)
{
	return Upper(area) + "/" + Lower(object);
}

bool TrapTable::Load(const TableMgr& table)
{
	rows.clear();
	static const char* const columns[] = { "AREA", "OBJECT", "KIND", "DISARM", "DEADLY", "XP", "NOTICE" };
	TableMgr::index_t cols[7];
	for (int i = 0; i < 7; ++i) {
		cols[i] = table.GetColumnIndex(std::string(columns[i]));
		if (cols[i] == TableMgr::npos) return false;
	}
	for (TableMgr::index_t r = 0; r < table.GetRowCount(); ++r) {
		TrapInfo info;
		info.area = Upper(table.QueryField(r, cols[0]));
		info.object = Lower(table.QueryField(r, cols[1]));
		if (!ParseTrapKind(table.QueryField(r, cols[2]), info.kind)) return false;
		std::string disarm = Upper(table.QueryField(r, cols[3]));
		if (disarm == "NONE") {
			info.disarmable = false;
		} else if (disarm == "AGI") {
			info.disarm = Ability::Agi;
		} else if (disarm == "STR") {
			info.disarm = Ability::Str;
		} else if (disarm == "INT") {
			info.disarm = Ability::Int;
		} else {
			return false;
		}
		info.deadly = atoi(table.QueryField(r, cols[4]).c_str()) != 0;
		info.xp = atoi(table.QueryField(r, cols[5]).c_str());
		info.notice = Spaced(table.QueryField(r, cols[6]));
		Set(info);
	}
	return true;
}

void TrapTable::Set(const TrapInfo& info)
{
	rows[TrapKey(info.area, info.object)] = info;
}

const TrapInfo* TrapTable::Find(const std::string& area, const std::string& object) const
{
	auto it = rows.find(TrapKey(area, object));
	return it == rows.end() ? nullptr : &it->second;
}

const char* ThiefRollerName(ThiefRoller roller)
{
	static const char* const names[] = { "thief", "halfling", "anyone", "trained d20", "familiar d10+2", "untrained d10" };
	size_t i = static_cast<size_t>(roller);
	return i < static_cast<size_t>(ThiefRoller::count) ? names[i] : "?";
}

bool SkillNeedsTools(ThiefSkill skill)
{
	switch (skill) {
		case ThiefSkill::PickLock:
		case ThiefSkill::FindTrap:
		case ThiefSkill::DisableTrap:
		case ThiefSkill::Climb:
		case ThiefSkill::Forge:
		case ThiefSkill::HandlePoison:
			return true;
		default:
			return false;
	}
}

bool PhysicalAbility(Ability ability)
{
	return ability == Ability::Str || ability == Ability::Agi;
}

bool HasThiefBonus(const ThiefCheckInput& in)
{
	if (in.thief) return in.tools || !SkillNeedsTools(in.skill);
	return in.halfling && (in.skill == ThiefSkill::Sneak || in.skill == ThiefSkill::Hide);
}

ThiefPlan PlanThiefCheck(const ThiefCheckInput& in)
{
	ThiefPlan plan;
	plan.die = 20;
	plan.ability = in.ability;
	if (HasThiefBonus(in)) {
		plan.roller = in.thief ? ThiefRoller::Thief : ThiefRoller::Halfling;
		plan.skill = in.tableBonus;
	} else {
		plan.roller = ThiefRoller::Anyone;
	}
	plan.abilityMod = in.abilityMod;
	plan.augur = in.augur;
	plan.armor = PhysicalAbility(in.ability) ? std::min(0, in.armor) : 0;
	plan.dwarf = in.skill == ThiefSkill::FindTrap ? std::max(0, in.dwarf) : 0;
	plan.situational = in.situational;
	return plan;
}

CheckOutcome BotchOutcome(int natural, int total, int dc)
{
	if (natural == 1) return CheckOutcome::Botch;
	if (total >= dc) return CheckOutcome::Success;
	return dc - total >= BOTCH_MARGIN ? CheckOutcome::Botch : CheckOutcome::Miss;
}

StealthTable::StealthTable()
{
	bands = { { 30, 5, "dim" }, { 50, 10, "moderate light" }, { 75, 15, "light, in shadow" }, { 100, 20, "broad daylight" } };
}

bool StealthTable::Load(const TableMgr& table)
{
	auto light = table.GetColumnIndex(std::string("LIGHT"));
	auto dc = table.GetColumnIndex(std::string("DC"));
	auto label = table.GetColumnIndex(std::string("LABEL"));
	if (light == TableMgr::npos || dc == TableMgr::npos || label == TableMgr::npos || !table.GetRowCount()) return false;
	std::vector<LightBand> list;
	for (TableMgr::index_t r = 0; r < table.GetRowCount(); ++r) {
		LightBand band;
		band.maxLight = atoi(table.QueryField(r, light).c_str());
		band.dc = atoi(table.QueryField(r, dc).c_str());
		band.label = Spaced(table.QueryField(r, label));
		if (!list.empty() && band.maxLight <= list.back().maxLight) return false;
		list.push_back(band);
	}
	bands = std::move(list);
	return true;
}

void StealthTable::SetBands(std::vector<LightBand> list)
{
	if (!list.empty()) bands = std::move(list);
}

const LightBand& StealthTable::Band(int light) const
{
	for (const LightBand& band : bands) {
		if (light <= band.maxLight) return band;
	}
	return bands.back();
}

std::string FloorLabel(int dc)
{
	switch (dc) {
		case 5:
			return "cushioned";
		case 10:
			return "stone";
		case 15:
			return "creaky wood";
		case 20:
			return "very noisy";
		default:
			return "DC " + std::to_string(dc);
	}
}

bool FloorTable::LoadAreas(const TableMgr& table)
{
	auto col = table.GetColumnIndex(std::string("FLOOR"));
	if (col == TableMgr::npos) return true; // an older table: every area has the default
	for (TableMgr::index_t r = 0; r < table.GetRowCount(); ++r) {
		const std::string& field = table.QueryField(r, col);
		if (field == "*" || field.empty()) continue;
		SetArea(table.GetRowName(r), atoi(field.c_str()));
	}
	return true;
}

bool FloorTable::LoadRegions(const TableMgr& table)
{
	auto area = table.GetColumnIndex(std::string("AREA"));
	auto region = table.GetColumnIndex(std::string("REGION"));
	auto dc = table.GetColumnIndex(std::string("DC"));
	if (area == TableMgr::npos || region == TableMgr::npos || dc == TableMgr::npos) return false;
	for (TableMgr::index_t r = 0; r < table.GetRowCount(); ++r) {
		SetRegion(table.QueryField(r, area), table.QueryField(r, region), atoi(table.QueryField(r, dc).c_str()));
	}
	return true;
}

void FloorTable::SetArea(const std::string& area, int dc)
{
	areas[Upper(area)] = dc;
}

void FloorTable::SetRegion(const std::string& area, const std::string& region, int dc)
{
	regions[Upper(area)][Lower(region)] = dc;
}

int FloorTable::AreaDC(const std::string& area) const
{
	auto it = areas.find(Upper(area));
	return it == areas.end() ? FLOOR_DC_DEFAULT : it->second;
}

const std::map<std::string, int>* FloorTable::Regions(const std::string& area) const
{
	auto it = regions.find(Upper(area));
	return it == regions.end() ? nullptr : &it->second;
}

int PickPocketDC(int targetStat, bool unaware, bool fighting)
{
	if (targetStat >= PICKPOCKET_NEVER) return -1;
	if (fighting) return PICKPOCKET_DC_WATCHFUL;
	if (unaware) return PICKPOCKET_DC_UNAWARE;
	return targetStat > 0 ? targetStat : PICKPOCKET_DC_DEFAULT;
}

const char* UnawareText(Unaware why)
{
	switch (why) {
		case Unaware::Hidden:
			return "hidden";
		case Unaware::Unheard:
			return "unheard";
		case Unaware::Unseen:
			return "unseen";
		case Unaware::NotFighting:
			return "not yet fighting";
		default:
			return "aware";
	}
}

Unaware TargetUnaware(bool alreadyAware, bool hidden, bool inSight, bool silent, bool targetFighting, bool behind)
{
	if (alreadyAware) return Unaware::No;
	if (hidden) return Unaware::Hidden;
	if (!inSight) return silent ? Unaware::Unheard : Unaware::Unseen;
	if (!targetFighting && behind) return Unaware::NotFighting;
	return Unaware::No;
}

bool BackstabWeapon(bool melee, bool fists, bool hasBackstabDice)
{
	if (fists) return false;
	return melee || hasBackstabDice;
}

bool LosesTrack(int brokenAwayTicks, bool hidden, int lostTicks, int roundTicks)
{
	return hidden && brokenAwayTicks >= roundTicks && lostTicks >= roundTicks;
}

}
}
