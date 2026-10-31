// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Deeds.h
 * Mighty Deeds of Arms (docs/rules/combat.md; the RTwP policy in
 * docs/rules/open-questions.md): the seven deed categories, which deed an
 * attack makes (a queued one-off, the signature deed, a weapon's own), when it
 * succeeds and how strong it is, and the deed table.
 *
 * dccdeeds.2da (unhardcoded/ethren) has a row per category and deed die result,
 * named KEY3 .. KEY7 (BLIND3, TRIP7, ...; 7 stands for 7 and up), with the
 * crit table columns DICE, EFFECTS, SAVE and TEXT (CombatTables.h). A save of
 * REF:A is against the attack roll's total.
 */

#ifndef DCC_DEEDS_H
#define DCC_DEEDS_H

#include "exports.h"

#include "DCC/CombatTables.h"

#include <array>

namespace GemRB {

class TableMgr;

namespace DCC {

// IE_DCC_DEED (the signature deed) and IE_DCC_NEXTDEED (a one-off for the next attack)
enum class Deed : uint8_t {
	None,
	Blind,
	Disarm,
	Push,
	Trip,
	Precision, // a precision shot: extra damage
	Rally,
	Defend, // defensive maneuvers
	count
};
constexpr int DEED_COUNT = static_cast<int>(Deed::count);
// what a warrior or dwarf without a stored choice declares
constexpr Deed DEFAULT_SIGNATURE_DEED = Deed::Trip;

/** BLIND, DISARM, PUSH, TRIP, PRECISION, RALLY, DEFEND; "" for None */
GEM_EXPORT const char* DeedKey(Deed deed);
/** the row prefix of a deed table row; Deed::None for anything else */
GEM_EXPORT Deed ParseDeedKey(const std::string& key);

// the deed die has to show this much (and the attack hit) for the deed to happen
constexpr int DEED_SUCCESS = 3;
// the strongest row; higher results use it ("7+")
constexpr int DEED_MAX_RESULT = 7;
/** a deed succeeds on a hit with the deed die (without its fixed d10+N part) at 3 or more */
GEM_EXPORT bool DeedSucceeds(int deedRoll, bool hit);
/** the table row a deed die result gives: 0 below 3 (no deed), then 3..7 (7 for 7 and up) */
GEM_EXPORT int DeedStrength(int deedRoll);

/** the deed an attack declares: a queued one-off first, then the signature deed, then the
 * weapon's own (a flail trips, a warhammer pushes) */
GEM_EXPORT Deed ChooseDeed(Deed queued, Deed signature, Deed weaponDeed);
/** the deed a weapon makes when nothing else is declared, by IE item type (itemtype.ids):
 * flails (23) trip, hammers (21) push; None for the rest */
GEM_EXPORT Deed WeaponDeed(int itemType);

class GEM_EXPORT DeedTable {
public:
	/** rows KEY3..KEY7 for every category but None */
	bool Load(const TableMgr& table);
	/** sets one category's rows (rolls 3..7, in any order) */
	bool SetRows(Deed deed, std::vector<ResultRow> rows);
	bool IsEmpty() const;
	/** the row for a deed and a deed die result (clamped to 3..7); nullptr below 3 or for None */
	const ResultRow* Lookup(Deed deed, int deedRoll) const;

private:
	std::array<ResultTable, DEED_COUNT> tables;
};

}
}

#endif
