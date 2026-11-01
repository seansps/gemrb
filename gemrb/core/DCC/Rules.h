// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Rules.h
 * The DCC tables a game with DCCRules loads at startup (core->GetDCCRules()),
 * plus the mapping from the five IE saving throw types to Fort/Ref/Will.
 *
 * dccsave.2da has rows SPELL, BREATH, DEATH, WAND, POLYMORPH and DEFAULT
 * and a SAVE column (FORT, REF or WILL). Stock IE effects name 2e save types,
 * so this decides which DCC save they roll.
 */

#ifndef DCC_RULES_H
#define DCC_RULES_H

#include "exports.h"

#include "DCC/Augur.h"
#include "DCC/Character.h"
#include "DCC/Classes.h"
#include "DCC/CombatTables.h"
#include "DCC/Funnel.h"
#include "DCC/Deeds.h"
#include "DCC/Body.h"
#include "DCC/Looks.h"
#include "DCC/Learning.h"
#include "DCC/Magic.h"

#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace GemRB {

class TableMgr;

namespace DCC {

// the IE save types in the order of Effect::SavingThrowType bits and Actor::GetSavingThrow
enum class IESave : uint8_t {
	Spell,
	Breath,
	Death,
	Wand,
	Polymorph,
	count
};
constexpr int IE_SAVE_COUNT = static_cast<int>(IESave::count);

class GEM_EXPORT SaveMap {
public:
	SaveMap();
	/** reads a 2DA with SPELL..POLYMORPH (and optional DEFAULT) rows and a SAVE column;
	 * missing rows take the DEFAULT row, or Will */
	bool Load(const TableMgr& table);
	void Set(IESave type, Save save) { map[static_cast<size_t>(type)] = save; }
	Save Get(IESave type) const;
	Save Get(unsigned int ieType) const;
	Save Default() const { return defaultSave; }

	/** FORT, REF, WILL; false for anything else */
	static bool ParseSave(const std::string& key, Save& save);
	static const char* SaveKey(Save save);

private:
	std::array<Save, IE_SAVE_COUNT> map;
	Save defaultSave = Save::Will;
};

class GEM_EXPORT RulesData {
public:
	XPTable xp;
	ClassTable classes;
	AugurTable augurs;
	SaveMap saves;
	ThiefSkillTable thiefSkills;
	CombatTables combat;
	DeedTable deeds;
	MagicTables magic;
	SpellLists spellLists; // Tables 5-8 and 5-9 (dccspls.2da), Phase 3's level-1 class pick
	ClericSpellsTable clericSpells; // Table 1-5's spells known (dccclesp.2da)
	BodyTable body; // M2.10: body weight by race and the load limits (dccbody.2da)
	HairTable hair; // M2.11: the hair and beard overlays (dcchair.2da)
	GodTable gods; // M3.3: the gods a cleric may serve by alignment (dccgods.2da)

	/** loads every table through gamedata; logs and returns false if one is missing or bad */
	bool Load();
	/** a DCC spell by its base resref (listed in the game's dccspell.2da, with a table of its
	 * own of the same name), loaded on first use; nullptr for any other spell */
	const SpellInfo* Spell(const std::string& resref) const;
	/** the base resrefs dccspell.2da lists */
	std::vector<std::string> SpellIndex() const;

private:
	mutable std::map<std::string, std::unique_ptr<SpellInfo>> spells;
	mutable std::set<std::string> spellIndex;
	mutable bool spellIndexLoaded = false;
};

}
}

#endif
