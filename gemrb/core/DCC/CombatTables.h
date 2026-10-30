// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file CombatTables.h
 * The DCC combat tables (docs/rules/crits-fumbles.md, combat.md Table 4-3,
 * equipment.md Table 3-3), all in unhardcoded/ethren:
 *
 * - dcccrit1..5.2da (Crit Tables I-V), dcccritm.2da (Crit Table M, monsters)
 *   and dccfumbl.2da (Table 4-2): a row per roll (the first row also stands
 *   for anything lower, the last for anything higher) with the columns
 *   DICE (extra damage, "2d6+1" or *), EFFECTS (see below), SAVE (the save
 *   the ?/! effects depend on: FORT:15:L is a Fort save vs DC 15 + the
 *   attacker's level, or *) and TEXT (a few words for the combat log, with
 *   _ for spaces).
 * - dccmcrit.2da (Table 9-10): rows by monster HD (0 is less than 1, the
 *   last row also counts for more), a column per monster type with the
 *   crit table and dice, e.g. III/d8 or V/2d10, - for none.
 * - dccfumdi.2da: the fumble die of an armor or shield, by item resref or
 *   by IE armor animation (ANIM_2A ...), and SHIELD for any shield.
 * - dcctwowp.2da (Table 4-3): rows by the highest Agility of the band, with
 *   the PRIMARY and OFF dice steps and the CRIT rule of the primary hand.
 *
 * EFFECTS: * or terms joined by +. A term may be a list of alternatives
 * joined by | (the first that applies wins, e.g. SHIELD|STUN:1d4 when the
 * target has no shield). A term is KEY[:ARG...][@COND], prefixed with ? when
 * it only applies on a failed save and ! when only on a passed one. Durations
 * are dice in rounds, with an optional unit (T turns, H hours, D days,
 * W weeks), PERM (permanent) or HEAL (until healed: the next night's rest).
 * The keys the engine knows are listed in docs/architecture/rules-dcc.md;
 * TODO:<what> marks a result the engine can't express yet (logged only).
 */

#ifndef DCC_COMBATTABLES_H
#define DCC_COMBATTABLES_H

#include "exports.h"

#include "DCC/Character.h"
#include "DCC/Combat.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace GemRB {

class TableMgr;

namespace DCC {

// XdY+Z; count 0 means none
struct DiceExpr {
	int count = 0;
	int sides = 0;
	int add = 0;
	bool IsNone() const { return count == 0 && add == 0; }
};
/** parses "2d6", "1d10+5", "d4", "3" (a fixed number) and "*" (none); false on anything else */
GEM_EXPORT bool ParseDice(const std::string& text, DiceExpr& dice);
/** rolls each die with roller and adds the fixed part */
GEM_EXPORT int RollDice(const DiceExpr& dice, const Roller& roller);
GEM_EXPORT std::string DiceString(const DiceExpr& dice);

// a duration from a table: rounds, a game time unit, permanent or until healed
struct Duration {
	enum class Kind : uint8_t {
		Rounds,
		Turns, // 10 minutes
		Hours,
		Days,
		Weeks,
		Permanent,
		UntilHealed
	};
	Kind kind = Kind::Rounds;
	DiceExpr dice;
};
/** "1d4", "2", "1d6D", "PERM", "HEAL"; empty is 1 round */
GEM_EXPORT bool ParseDuration(const std::string& text, Duration& duration);
/** seconds of game time for a rolled amount of units (a round is roundSeconds) */
GEM_EXPORT int DurationSeconds(Duration::Kind kind, int amount, int roundSeconds);

struct TableTerm {
	char gate = 0; // 0 always, '?' on a failed save, '!' on a passed save
	std::string key;
	std::vector<std::string> args;
	std::string condition; // after @, e.g. NOHELM, ARMORED
};
using TableAlternatives = std::vector<TableTerm>; // the first one that applies wins

struct SaveSpec {
	Save save = Save::Fort;
	int dc = 0;
	bool plusLevel = false; // + the attacker's level
	bool IsNone() const { return dc == 0; }
};
/** FORT:15:L, REF:10, WILL:20, or * */
GEM_EXPORT bool ParseSaveSpec(const std::string& text, SaveSpec& save);
/** parses the EFFECTS grammar above */
GEM_EXPORT bool ParseEffects(const std::string& text, std::vector<TableAlternatives>& effects);

struct ResultRow {
	int roll = 0;
	DiceExpr dice;
	std::vector<TableAlternatives> effects;
	SaveSpec save;
	std::string text; // underscores already turned into spaces
};

// a crit or fumble table: consecutive rolls, clamped at both ends
class GEM_EXPORT ResultTable {
public:
	/** rows must have consecutive rolls */
	bool SetRows(std::vector<ResultRow> newRows);
	bool Load(const TableMgr& table);
	bool IsEmpty() const { return rows.empty(); }
	int MinRoll() const { return rows.empty() ? 0 : rows.front().roll; }
	int MaxRoll() const { return rows.empty() ? 0 : rows.back().roll; }
	/** the row for a roll, clamped to the table (nullptr if empty) */
	const ResultRow* Lookup(int roll) const;

private:
	std::vector<ResultRow> rows;
};

// crit tables: I-V for characters (IE_DCC_CRITTABLE 1-5), M and the other monster tables
enum class CritTableID : uint8_t {
	None = 0,
	I = 1,
	II,
	III,
	IV,
	V,
	M, // monsters
	DR, // dragons
	DN, // demons
	G, // giants
	U, // un-dead
	count
};
/** "I" .. "V", "M", "DR", "DN", "G", "U" */
GEM_EXPORT const char* CritTableName(CritTableID id);
GEM_EXPORT CritTableID ParseCritTableName(const std::string& name);

// monster crits by HD and type (Table 9-10)
enum class MonsterType : uint8_t {
	Other, // everything else: Crit Table M
	Humanoid, // humanoids with weapons (goblins, orcs, ...): the PC tables
	Dragon,
	Demon,
	Giant,
	Undead,
	count
};
constexpr int MONSTER_TYPE_COUNT = static_cast<int>(MonsterType::count);
GEM_EXPORT const char* MonsterTypeKey(MonsterType type);

struct MonsterCrit {
	CritTableID table = CritTableID::None; // None: can't crit
	int dice = 0;
	int die = 0;
};
/** "III/d8", "V/2d10", "-" */
GEM_EXPORT bool ParseMonsterCrit(const std::string& text, MonsterCrit& crit);

class GEM_EXPORT MonsterCritTable {
public:
	using Row = std::array<MonsterCrit, MONSTER_TYPE_COUNT>;
	/** rows[hd], hd 0 (less than 1) up */
	bool SetRows(std::vector<Row> newRows);
	bool Load(const TableMgr& table);
	bool IsEmpty() const { return rows.empty(); }
	/** hd clamped to the table */
	MonsterCrit Lookup(int hd, MonsterType type) const;

private:
	std::vector<Row> rows;
};

// fumble dice of armor and shields (Table 3-3; the larger of the two counts)
class GEM_EXPORT ArmorFumbleTable {
public:
	bool Load(const TableMgr& table);
	void Set(const std::string& key, int die) { dice[key] = die; }
	bool IsEmpty() const { return dice.empty(); }
	/** the die of an item: its resref first, then its armor animation (ANIM_2A ...); 0 if unknown */
	int ArmorDie(const std::string& resref, const std::string& animation) const;
	/** the die of any shield (the SHIELD row, or an entry for the resref) */
	int ShieldDie(const std::string& resref) const;

private:
	int Find(const std::string& key) const;
	std::map<std::string, int> dice; // upper case keys
};

// two-weapon fighting (Table 4-3)
struct TwoWeaponRow {
	int maxAgility = 0; // the band's highest Agility
	int primarySteps = 0;
	int offSteps = 0;
	CritRule primaryCrit = CritRule::None;
};
struct TwoWeaponDice {
	int primarySteps = 0;
	int offSteps = 0;
	CritRule primaryCrit = CritRule::None;
	CritRule offCrit = CritRule::None;
	bool keepThreat = false; // the expanded threat range is lost with two weapons
};
// halflings count as Agi 16 at least, and a natural 16 is an automatic hit and crit (classes.md)
constexpr int HALFLING_TWO_WEAPON_AGILITY = 16;

class GEM_EXPORT TwoWeaponTable {
public:
	/** rows in rising maxAgility order */
	bool SetRows(std::vector<TwoWeaponRow> newRows);
	bool Load(const TableMgr& table);
	bool IsEmpty() const { return rows.empty(); }
	/** the dice for an Agility score; halflings use their class exception */
	TwoWeaponDice Lookup(int agility, bool halfling) const;

private:
	std::vector<TwoWeaponRow> rows;
};
/** NONE, MAX, MAX_AUTO, NORMAL */
GEM_EXPORT bool ParseCritRule(const std::string& text, CritRule& rule);

// everything the combat code reads (RulesData::combat)
struct GEM_EXPORT CombatTables {
	std::array<ResultTable, static_cast<size_t>(CritTableID::count)> crits; // by CritTableID
	ResultTable fumbles;
	MonsterCritTable monsterCrits;
	ArmorFumbleTable armorFumbles;
	TwoWeaponTable twoWeapons;

	/** the crit table to roll on; monster tables that aren't shipped fall back to M */
	const ResultTable* Crit(CritTableID id) const;
	/** loads every table through gamedata; logs and returns false if one is missing or bad */
	bool Load();
};

}
}

#endif
