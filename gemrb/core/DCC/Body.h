// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Body.h
 * Encumbrance by body weight (docs/rules/equipment.md, "Encumbrance"; an Ethren decision, M2.10).
 *
 * The book slows a character who carries "a substantial ratio" of its body weight to half
 * speed and lets nobody carry more than half of it. Ethren keeps GemRB's item weights and
 * reads the rest from unhardcoded/ethren/dccbody.2da: per race the body weight roll (BASE
 * pounds plus DICE d SIDES) and the two limits as percentages of the body weight (HALF:
 * more than this is half speed; CAP: more than this and the character can't move).
 * A character's weight is the stat IE_DCC_WEIGHT, rolled when it joins the party (or set
 * by the funnel's rolling screen); 0 means not rolled, and the race's average stands in.
 */

#ifndef DCC_BODY_H
#define DCC_BODY_H

#include "exports.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace GemRB {

class TableMgr;

namespace DCC {

enum class Load : uint8_t {
	Light, // normal speed
	Heavy, // half speed
	Over, // more than the cap: can't move
	count
};
GEM_EXPORT const char* LoadName(Load load);

struct BodyRow {
	int race = 1; // the IE race (race.ids): 1 human, 2 elf, 4 dwarf, 5 halfling
	int base = 0; // pounds
	int dice = 0;
	int sides = 0;
	int halfPercent = 33; // carrying more than this share of the body weight: half speed
	int capPercent = 50; // carrying more than this: can't move (the book: at most half the body weight)
};

struct LoadLimits {
	int half = 0; // pounds: more is half speed
	int cap = 0; // pounds: more and the character can't move
};

class GEM_EXPORT BodyTable {
public:
	/** dccbody.2da: rows by race name, columns RACE BASE DICE SIDES HALF CAP */
	bool Load(const TableMgr& table);
	void Set(const BodyRow& row);
	/** the race's row; an unknown race uses the first row (humans) */
	const BodyRow& Row(int race) const;
	/** BASE + the dice's average, rounded down */
	int Average(int race) const;
	/** BASE + DICE d SIDES, die(sides) rolling each die */
	int Roll(int race, const std::function<int(int)>& die) const;
	bool Empty() const { return rows.empty(); }

private:
	std::vector<BodyRow> rows;
};

/** the limits for a body weight: its HALF and CAP percent, rounded down */
GEM_EXPORT LoadLimits LimitsFor(int bodyWeight, const BodyRow& row);
/** carried pounds against the limits: up to half light, up to the cap heavy, past it over */
GEM_EXPORT Load LoadFor(int carried, const LoadLimits& limits);
/** what the IE movement rate is divided by: 1 light, 2 heavy, 0 (can't move) over */
GEM_EXPORT int SpeedDivisor(Load load);

}
}

#endif
