// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Learning.h
 * Which spells a DCC caster knows, as pure functions (docs/rules/magic.md "Who learns what",
 * classes.md Tables 1-5, 1-12 and 1-16, abilities.md Table 1-1; the Ethren decisions in
 * docs/rules/open-questions.md): how many spells a wizard, elf or cleric may know, and the
 * picks when one gains a level: random on Table 5-8 for wizards and elves, the god's list
 * (Table 5-9) for clerics.
 *
 * Only some spells have content yet (the game's dccspell.2da), so a pick that lands on a row
 * without content is substituted from the spells that exist, and when none is left the slot
 * stays open (owed) until content arrives. Every such case is reported in the pick's note.
 *
 * The tables are unhardcoded/ethren/dccspls.2da (Tables 5-8 and 5-9, each row with its spell's
 * base resref once it has content) and dccclesp.2da (Table 1-5's spells known by level). The
 * engine glue is Actor::DCCLearnLevelSpells (Scriptable/ActorDCCMagic.cpp).
 */

#ifndef DCC_LEARNING_H
#define DCC_LEARNING_H

#include "exports.h"

#include "DCC/Character.h"
#include "DCC/Magic.h"

#include <array>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace GemRB {

class TableMgr;

namespace DCC {

constexpr int SPELL_LEVELS = 5;

// dccspls.2da FLAGS
enum SpellListFlags : uint8_t {
	LIST_NONE = 0,
	LIST_CLERIC_SPELL = 1, // Table 5-8 *: the cleric spell, cast by a wizard at -2 (not in the engine yet)
	LIST_PATRON_PAIR = 2, // Table 5-8 **: invoke patron and patron bond, both for one slot
	LIST_PATRON_SPELL = 4 // Table 5-8 ***: the patron's spell of that level, re-rolled without patron bond
};

struct SpellListEntry {
	int row = 0; // the table's row, 1-based
	int level = 1;
	std::string name; // as the book has it, eg. "Chill touch"
	std::string spell; // the base resref once it has content, else ""
	uint8_t flags = LIST_NONE;
};

class GEM_EXPORT SpellLists {
public:
	/** dccspls.2da: rows named W1_01 (wizard, level 1, row 1) .. C5_03, columns LEVEL, ROW, SPELL, FLAGS
	 * (CLERIC, PAIR, PATRON or *) and NAME; false (and empty) if a row is malformed */
	bool Load(const TableMgr& table);
	/** Table 5-8 (CasterKind::Wizard) or 5-9 (CasterKind::Cleric) for a spell level, in row order */
	const std::vector<SpellListEntry>& List(CasterKind kind, int level) const;
	/** the entry that names a spell, nullptr if no list does */
	const SpellListEntry* Find(CasterKind kind, const std::string& spell) const;
	bool IsEmpty() const;
	void Set(CasterKind kind, int level, std::vector<SpellListEntry> list);

private:
	std::array<std::array<std::vector<SpellListEntry>, SPELL_LEVELS>, 2> lists;
};

// Table 1-5: a cleric's spells known by cleric level (1-10) and spell level (1-5)
class GEM_EXPORT ClericSpellsTable {
public:
	/** dccclesp.2da: rows 1-10, columns L1-L5 */
	bool Load(const TableMgr& table);
	void Set(int level, std::array<int, SPELL_LEVELS> counts);
	/** spells of spellLevel a cleric of level may know, 0 outside the table */
	int Known(int level, int spellLevel) const;

private:
	std::vector<std::array<int, SPELL_LEVELS>> rows; // rows[0] is level 1
};

/** wizards and elves: the class row's spells known + Table 1-1's Int adjustment, at least 1
 * (Table 1-1's minimum); 0 if the class has none or the Int can't cast */
GEM_EXPORT int ArcaneSpellsKnown(int classKnown, int intAdjustment, bool canCast);

// one slot of a level's picks
struct LearnPick {
	int level = 1;
	int row = 0; // the row rolled on the list (wizards, elves; 0 for a cleric's choice)
	std::string rolled; // the row's name
	std::string spell; // what the caster learns ("" an owed slot: nothing left with content)
	bool substituted = false; // the row has no content yet: spell is another of that level's
	std::string note; // what happened, for the log and the learn window
};

/** a wizard's or elf's random picks on Table 5-8: slots spells of level. Duplicates (of what it
 * knows, or of this roll) and the patron's spells (without patron bond) are re-rolled; a row
 * without content (or a cleric spell: wizards can't cast those yet) is noted and substituted
 * at random by a spell of that level it can learn (available); if none is left the slot is owed */
GEM_EXPORT std::vector<LearnPick> RollArcaneSpells(const SpellLists& lists, int level, int slots, const std::set<std::string>& known,
						     const std::function<bool(const std::string&)>& available, bool patronBond, const Roller& roller);
/** a cleric's picks from the god's list (Table 5-9): clerics choose, so these are only the
 * default: slots spells of level at random among the ones with content it doesn't know; the
 * rest owed */
GEM_EXPORT std::vector<LearnPick> PickClericSpells(const SpellLists& lists, int level, int slots, const std::set<std::string>& known,
						     const std::function<bool(const std::string&)>& available, const Roller& roller);

/** how many of a fresh level's random spells a wizard or elf may swap for a spell of its choice
 * (the book's optional rule: up to half, rounded down) */
GEM_EXPORT int ReplaceableSpells(int freshSpells);

}
}

#endif
