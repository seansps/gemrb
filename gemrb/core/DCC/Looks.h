// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Looks.h
 * How DCC characters look in the engine (M2.11, docs/architecture/art-pipeline.md, "Sprites").
 *
 * Hair and beards are overlay layers over the body, drawn like the helmet overlay, so every
 * species and gender can take any style without more animation sets. A character's styles are
 * the stats IE_DCC_HAIR and IE_DCC_BEARD (kept in the CRE extension's CHAR record), 0 for none
 * (a body with its hair baked in). unhardcoded/ethren/dcchair.2da gives each style id its
 * overlay: CODE is the overlay's 2-letter animation code (its BAMs are WQ<size><code><suffix>,
 * like a helmet's), HELMET what is drawn while a helmet is worn, a cropped style's code or * for
 * nothing. KIND is HAIR or BEARD. The layers take the body's palette: the hair range colours them.
 *
 * The weapon, off-hand and helmet overlays take their own colour bytes in DCC games, as in the
 * IE games: byte 1, 2 or 3 of each CRE colour (effect 7's locations 0x10-0x36), so an item's
 * equipping effects colour it. Stock GemRB shifts the colours by 8 x the palette type, past the
 * last byte, so every overlay came out in gradient 0.
 */

#ifndef DCC_LOOKS_H
#define DCC_LOOKS_H

#include "exports.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GemRB {

class TableMgr;

namespace DCC {

struct HairStyle {
	int id = 0;
	bool beard = false;
	std::string code; // the overlay's animation code, 2 letters
	std::string underHelmet; // the code drawn under a helmet, "" for nothing
};

class GEM_EXPORT HairTable {
public:
	/** dcchair.2da: rows by name, columns ID KIND (HAIR or BEARD) CODE HELMET */
	bool Load(const TableMgr& table);
	void Set(const HairStyle& style);
	/** the style of this kind with this id, nullptr for 0 or an unknown one */
	const HairStyle* Find(int id, bool beard) const;
	bool Empty() const { return styles.empty(); }

private:
	std::vector<HairStyle> styles;
};

/** the overlay to draw for a style: its code, or the helmet code while a helmet is worn ("" none) */
GEM_EXPORT std::string HairOverlay(const HairStyle* style, bool helmet);

/** the CRE colour byte an overlay's palette takes (CharAnimations' PaletteType: 5 weapon, 6
 * off-hand, 7 helmet take bytes 1-3; the body's parts take byte 0) */
GEM_EXPORT unsigned int OverlayColourByte(unsigned int paletteType);

/** the IE games' stances: a character stands ready (G1) while it is engaged in melee or its
 * party is in a fight, awake (G17) otherwise */
GEM_EXPORT bool CombatStance(bool engaged, bool partyFight);

}
}

#endif
