// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Funnel.h
 * The end of the level-0 funnel (M3.3, "choose your four and the reserves"), the parts with no
 * engine dependency: the gods a cleric may serve by alignment (dccgods.2da), when a peasant nearly
 * died, and how many go on. The game's graveyard is in Extension.h (GameRecord); the party
 * changes are Game::DCCEndFunnel, DCCDismiss and DCCRecruit.
 */

#ifndef DCC_FUNNEL_H
#define DCC_FUNNEL_H

#include "exports.h"

#include "DCC/Character.h"

#include <string>
#include <vector>

namespace GemRB {

class TableMgr;

namespace DCC {

// the party after the funnel: four characters (Ethren decision, docs/rules/open-questions.md)
constexpr int FUNNEL_PARTY_SIZE = 4;
// where the reserves wait: no area has this name, so they are on no map until they join
constexpr const char* RESERVE_AREA = "DCCRESV";

struct God {
	int id = 0;
	Alignment alignment = Alignment::Neutral;
	std::string name;
	std::string dcc; // the DCC core god it stands in for, a key ("Ahriman", "Three_Fates"): its mechanics
	std::string domain; // what it is the god of ("death, dark magic")
};

class GEM_EXPORT GodTable {
public:
	/** dccgods.2da: rows by name, columns ID ALIGN (L, N or C) NAME, and DCC (the DCC core god it
	 * stands in for) and DOMAIN where the table has them; _ for spaces in NAME and DOMAIN */
	bool Load(const TableMgr& table);
	void Set(const God& god);
	/** the god with this id, nullptr for 0 or an unknown one */
	const God* Find(int id) const;
	/** the gods of an alignment, in the table's order */
	std::vector<const God*> For(Alignment alignment) const;
	/** a god that follows an alignment change: kept if it is of that alignment, else 0 (none) */
	int Follow(int god, Alignment alignment) const;
	/** the first god of an alignment (a cleric made without a choice), 0 if it has none */
	int Default(Alignment alignment) const;
	bool Empty() const { return gods.empty(); }

private:
	std::vector<God> gods;
};

/** a funnel record's near death: the hit points fell below 2 from 2 or more and it still stands */
GEM_EXPORT bool NearDeath(int oldHP, int newHP);
/** how many go on from the funnel: four, or every survivor if fewer live */
GEM_EXPORT int FunnelPicks(int survivors);
/** the alignment of FunnelRoll's codes (1 lawful, 2 neutral, 3 chaotic) as IE_ALIGNMENT, 0 for another code */
GEM_EXPORT unsigned int AlignmentCodeToIE(int code);
/** IE_ALIGNMENT as FunnelRoll's code (1-3) */
GEM_EXPORT int AlignmentCode(unsigned int ieAlignment);

}
}

#endif
