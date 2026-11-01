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
#include <map>
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

/**
 * Weapons held in both hands although DCC counts them one-handed (the staff, spear, pitchfork,
 * shovel and pole: d20 initiative, a shield allowed; docs/rules/equipment.md). Only the animation
 * changes: wielded with nothing in the off hand they take the two-handed stances and attack files
 * (the IE character layout's G13 ready cycles and A2 A4 A6); with a shield or an off-hand weapon the
 * one-handed ones. The true two-handed weapons (the ITM's two-handed flag, which the d16 initiative
 * and the shield rule read) take the two-handed files anyway.
 * unhardcoded/ethren/dcchold.2da: rows by an item's 2-letter animation code (ITM 0x22) or an item
 * resref (which wins over its code's row), column HANDS 2 (held in both hands) or 1 (one-handed).
 */
class GEM_EXPORT HeldTable {
public:
	bool Load(const TableMgr& table);
	/** a row: an animation code or an item resref */
	void Set(const std::string& key, bool bothHands);
	/** whether this item (resref, animation code) is held in both hands while the off hand is free */
	bool BothHands(const std::string& resref, const std::string& animCode) const;
	bool Empty() const { return rows.empty(); }

private:
	std::map<std::string, bool> rows; // upper case
};

/** what the off hand holds, for the weapon's animation */
enum class OffHand : uint8_t { Empty, Shield, Weapon };

/** the animation's weapon type (CharAnimations' IE_ANI_WEAPON_1H 0, 2H 1, 2W 2) for the weapon in
 * hand in DCC games: the ITM's two-handed flag gives 2H and an off-hand weapon 2W (both as stock
 * GemRB); a weapon held in both hands (HeldTable) takes 2H while the off hand is empty; else 1H */
GEM_EXPORT unsigned char WeaponAnimType(bool twoHandedFlag, bool heldInBothHands, OffHand offHand);

}
}

#endif
