// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Augur.h
 * DCC birth augurs (Table 1-2, docs/rules/luck.md). Each character has one of
 * 30 augurs and a Luck modifier frozen at creation; that modifier applies to
 * the rolls the augur names ("lucky roll").
 *
 * The data is unhardcoded/ethren/dccaugur.2da: rows 1-30, a NAME column and a
 * TARGET key (ATTACK_ALL, SAVE_FORT, AC, ...). Code asks whether an augur
 * applies to a roll with AugurApplies; Actor::GetDCCAugurBonus is the hook
 * the game uses (saves, AC, HP, initiative and speed in M2.2, attacks in
 * M2.3, spell checks in M2.5, ...).
 */

#ifndef DCC_AUGUR_H
#define DCC_AUGUR_H

#include "exports.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GemRB {

class TableMgr;

namespace DCC {

// what an augur's lucky roll is (the TARGET column)
enum class AugurTarget : uint8_t {
	None,
	AttackAll, // 1 harsh winter
	AttackMelee, // 2 the bull
	AttackMissile, // 3 fortunate date
	AttackUnarmed, // 4 raised by wolves
	AttackMounted, // 5 conceived on horseback
	DamageAll, // 6 born on the battlefield
	DamageMelee, // 7 path of the bear
	DamageMissile, // 8 hawkeye
	OccupationWeapon, // 9 pack hunter: attack and damage with the 0-level weapon
	SkillChecks, // 10 born under the loom (thief skills too)
	Traps, // 11 fox's cunning: find/disable traps
	SecretDoors, // 12 four-leafed clover
	SpellChecks, // 13 seventh son
	SpellDamage, // 14 the raging storm
	TurnUnholy, // 15 righteous heart
	Healing, // 16 survived the plague
	SavesAll, // 17 lucky sign
	SavesTraps, // 18 guardian angel
	SavesPoison, // 19 survived a spider bite
	SaveRef, // 20 struck by lightning
	SaveFort, // 21 lived through famine
	SaveWill, // 22 resisted temptation
	ArmorClass, // 23 charmed house
	Initiative, // 24 speed of the cobra
	HitPoints, // 25 bountiful harvest (each level)
	CritTables, // 26 warrior's arm
	Corruption, // 27 unholy house
	Fumbles, // 28 the broken star
	Languages, // 29 birdsong
	Speed, // 30 wild child: 5 ft per point
	count
};

/** the 2DA key of a target ("ATTACK_ALL" ...) and back; unknown keys give None */
GEM_EXPORT const char* AugurTargetKey(AugurTarget target);
GEM_EXPORT AugurTarget ParseAugurTarget(const std::string& key);

// the kind of roll a caller asks about
enum class AugurRoll : uint8_t {
	Attack,
	Damage,
	SkillCheck,
	FindTrap,
	DisableTrap,
	SecretDoor,
	SpellCheck,
	SpellDamage,
	TurnUnholy,
	Healing,
	Save,
	ArmorClass,
	Initiative,
	HitPoints,
	CritTable,
	Corruption,
	Fumble,
	Languages,
	Speed
};

// details of the roll; unarmed melee attacks pass AF_MELEE | AF_UNARMED
enum AugurFlags : uint32_t {
	AF_NONE = 0,
	AF_MELEE = 1,
	AF_MISSILE = 2,
	AF_UNARMED = 4,
	AF_MOUNTED = 8,
	AF_OCCUPATION_WEAPON = 16,
	AF_FORT = 32,
	AF_REF = 64,
	AF_WILL = 128,
	AF_TRAP = 256, // a save against a trap
	AF_POISON = 512 // a save against poison
};

/** true if an augur with this target is the lucky roll for roll (with flags) */
GEM_EXPORT bool AugurApplies(AugurTarget target, AugurRoll roll, uint32_t flags = AF_NONE);
/** frozenMod if the augur applies, else 0 */
GEM_EXPORT int AugurBonus(AugurTarget target, int frozenMod, AugurRoll roll, uint32_t flags = AF_NONE);

struct AugurRow {
	int roll = 0; // 1-30
	std::string name;
	AugurTarget target = AugurTarget::None;
};

class GEM_EXPORT AugurTable {
public:
	/** rows must be numbered 1..n once each, with known targets, else the table stays empty */
	bool SetRows(std::vector<AugurRow> newRows);
	/** reads a 2DA with d30 row names and NAME and TARGET columns */
	bool Load(const TableMgr& table);

	bool IsEmpty() const { return rows.empty(); }
	int Count() const { return static_cast<int>(rows.size()); }
	/** an augur outside the table (0: none) has no target and an empty name */
	AugurTarget Target(int augur) const;
	const std::string& Name(int augur) const;

private:
	std::vector<AugurRow> rows; // rows[i].roll == i + 1
};

}
}

#endif
