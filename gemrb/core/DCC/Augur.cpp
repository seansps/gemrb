// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Augur.h"

#include "TableMgr.h"
#include "globals.h"

#include <algorithm>
#include <array>

namespace GemRB {
namespace DCC {

static const std::array<const char*, static_cast<size_t>(AugurTarget::count)> targetKeys {
	"NONE",
	"ATTACK_ALL",
	"ATTACK_MELEE",
	"ATTACK_MISSILE",
	"ATTACK_UNARMED",
	"ATTACK_MOUNTED",
	"DAMAGE_ALL",
	"DAMAGE_MELEE",
	"DAMAGE_MISSILE",
	"OCCUPATION_WEAPON",
	"SKILL_CHECKS",
	"TRAPS",
	"SECRET_DOORS",
	"SPELL_CHECKS",
	"SPELL_DAMAGE",
	"TURN_UNHOLY",
	"HEALING",
	"SAVES_ALL",
	"SAVES_TRAPS",
	"SAVES_POISON",
	"SAVE_REF",
	"SAVE_FORT",
	"SAVE_WILL",
	"AC",
	"INITIATIVE",
	"HIT_POINTS",
	"CRIT_TABLES",
	"CORRUPTION",
	"FUMBLES",
	"LANGUAGES",
	"SPEED"
};

const char* AugurTargetKey(AugurTarget target)
{
	size_t i = static_cast<size_t>(target);
	return i < targetKeys.size() ? targetKeys[i] : targetKeys[0];
}

AugurTarget ParseAugurTarget(const std::string& key)
{
	for (size_t i = 0; i < targetKeys.size(); ++i) {
		if (key == targetKeys[i]) return static_cast<AugurTarget>(i);
	}
	return AugurTarget::None;
}

bool AugurApplies(AugurTarget target, AugurRoll roll, uint32_t flags)
{
	bool attack = roll == AugurRoll::Attack;
	bool damage = roll == AugurRoll::Damage;
	bool save = roll == AugurRoll::Save;

	switch (target) {
		case AugurTarget::None:
		case AugurTarget::count:
			return false;
		case AugurTarget::AttackAll:
			return attack;
		case AugurTarget::AttackMelee:
			return attack && (flags & AF_MELEE);
		case AugurTarget::AttackMissile:
			return attack && (flags & AF_MISSILE);
		case AugurTarget::AttackUnarmed:
			return attack && (flags & AF_UNARMED);
		case AugurTarget::AttackMounted:
			return attack && (flags & AF_MOUNTED);
		case AugurTarget::DamageAll:
			return damage;
		case AugurTarget::DamageMelee:
			return damage && (flags & AF_MELEE);
		case AugurTarget::DamageMissile:
			return damage && (flags & AF_MISSILE);
		case AugurTarget::OccupationWeapon:
			return (attack || damage) && (flags & AF_OCCUPATION_WEAPON);
		case AugurTarget::SkillChecks: // includes thief skills
			return roll == AugurRoll::SkillCheck || roll == AugurRoll::FindTrap || roll == AugurRoll::DisableTrap || roll == AugurRoll::SecretDoor;
		case AugurTarget::Traps:
			return roll == AugurRoll::FindTrap || roll == AugurRoll::DisableTrap;
		case AugurTarget::SecretDoors:
			return roll == AugurRoll::SecretDoor;
		case AugurTarget::SpellChecks:
			return roll == AugurRoll::SpellCheck;
		case AugurTarget::SpellDamage:
			return roll == AugurRoll::SpellDamage;
		case AugurTarget::TurnUnholy:
			return roll == AugurRoll::TurnUnholy;
		case AugurTarget::Healing:
			return roll == AugurRoll::Healing;
		case AugurTarget::SavesAll:
			return save;
		case AugurTarget::SavesTraps:
			return save && (flags & AF_TRAP);
		case AugurTarget::SavesPoison:
			return save && (flags & AF_POISON);
		case AugurTarget::SaveRef:
			return save && (flags & AF_REF);
		case AugurTarget::SaveFort:
			return save && (flags & AF_FORT);
		case AugurTarget::SaveWill:
			return save && (flags & AF_WILL);
		case AugurTarget::ArmorClass:
			return roll == AugurRoll::ArmorClass;
		case AugurTarget::Initiative:
			return roll == AugurRoll::Initiative;
		case AugurTarget::HitPoints:
			return roll == AugurRoll::HitPoints;
		case AugurTarget::CritTables:
			return roll == AugurRoll::CritTable;
		case AugurTarget::Corruption:
			return roll == AugurRoll::Corruption;
		case AugurTarget::Fumbles:
			return roll == AugurRoll::Fumble;
		case AugurTarget::Languages:
			return roll == AugurRoll::Languages;
		case AugurTarget::Speed:
			return roll == AugurRoll::Speed;
	}
	return false;
}

int AugurBonus(AugurTarget target, int frozenMod, AugurRoll roll, uint32_t flags)
{
	return AugurApplies(target, roll, flags) ? frozenMod : 0;
}

bool AugurTable::SetRows(std::vector<AugurRow> newRows)
{
	rows.clear();
	if (newRows.empty()) return false;

	std::sort(newRows.begin(), newRows.end(), [](const AugurRow& a, const AugurRow& b) {
		return a.roll < b.roll;
	});
	for (size_t i = 0; i < newRows.size(); ++i) {
		if (newRows[i].roll != int(i) + 1 || newRows[i].target == AugurTarget::None) return false;
	}

	rows = std::move(newRows);
	return true;
}

bool AugurTable::Load(const TableMgr& table)
{
	rows.clear();
	auto nameCol = table.GetColumnIndex("NAME");
	auto targetCol = table.GetColumnIndex("TARGET");
	if (nameCol == TableMgr::npos || targetCol == TableMgr::npos) {
		return false;
	}

	std::vector<AugurRow> newRows;
	for (TableMgr::index_t row = 0; row < table.GetRowCount(); ++row) {
		AugurRow entry;
		if (!valid_signednumber(table.GetRowName(row).c_str(), entry.roll)) return false;
		entry.name = table.QueryField(row, nameCol);
		entry.target = ParseAugurTarget(table.QueryField(row, targetCol));
		newRows.push_back(std::move(entry));
	}
	return SetRows(std::move(newRows));
}

AugurTarget AugurTable::Target(int augur) const
{
	if (augur < 1 || augur > Count()) return AugurTarget::None;
	return rows[augur - 1].target;
}

const std::string& AugurTable::Name(int augur) const
{
	static const std::string none;
	if (augur < 1 || augur > Count()) return none;
	return rows[augur - 1].name;
}

}
}
